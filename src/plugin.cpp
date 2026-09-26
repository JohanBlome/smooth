#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "face_detector.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("smooth", "en-US")

namespace {
constexpr const char *kSmoothStrength = "smooth_strength";
constexpr const char *kSmoothCenter = "smooth_center";
constexpr const char *kSmoothRadius = "smooth_radius";
constexpr const char *kSmoothChroma = "smooth_chroma";
constexpr const char *kEyeStrength = "eye_strength";
constexpr const char *kSharpenPosition = "sharpen_position";
constexpr const char *kSharpenAmount = "sharpen_amount";
constexpr const char *kSharpenRadius = "sharpen_radius";
constexpr const char *kSharpenChroma = "sharpen_chroma";
constexpr const char *kSharpenScope = "sharpen_scope";
constexpr const char *kDetectionRate = "detection_rate";
constexpr const char *kDebugView = "debug_view";

struct SmoothFilter {
  obs_source_t *source = nullptr;
  gs_effect_t *effect = nullptr;
  gs_texrender_t *capture = nullptr;
  gs_texrender_t *processing = nullptr;
  gs_texrender_t *detector_render = nullptr;
  gs_stagesurf_t *stage = nullptr;
  enum gs_color_format capture_format = GS_UNKNOWN;

  gs_eparam_t *image_param = nullptr;
  gs_eparam_t *texel_size_param = nullptr;
  std::array<gs_eparam_t *, kMaxFaces> face_center_params{};
  std::array<gs_eparam_t *, kMaxFaces> face_size_params{};
  std::array<gs_eparam_t *, kMaxFaces> left_eye_params{};
  std::array<gs_eparam_t *, kMaxFaces> right_eye_params{};
  std::array<gs_eparam_t *, kMaxFaces> detection_alpha_params{};
  gs_eparam_t *smooth_strength_param = nullptr;
  gs_eparam_t *smooth_center_param = nullptr;
  gs_eparam_t *smooth_radius_param = nullptr;
  gs_eparam_t *smooth_chroma_param = nullptr;
  gs_eparam_t *eye_strength_param = nullptr;
  gs_eparam_t *sharpen_amount_param = nullptr;
  gs_eparam_t *sharpen_radius_param = nullptr;
  gs_eparam_t *sharpen_chroma_param = nullptr;
  gs_eparam_t *sharpen_masked_param = nullptr;
  gs_eparam_t *debug_view_param = nullptr;

  std::unique_ptr<FaceDetector> detector;
  std::atomic<float> smooth_strength{0.55f};
  std::atomic<float> smooth_center{7.0f};
  std::atomic<float> smooth_radius{10.0f};
  std::atomic<float> smooth_chroma{1.0f};
  std::atomic<float> eye_strength{0.45f};
  std::atomic<int> sharpen_position{0};
  std::atomic<float> sharpen_amount{0.35f};
  std::atomic<float> sharpen_radius{1.0f};
  std::atomic<float> sharpen_chroma{0.0f};
  std::atomic<int> sharpen_scope{0};
  std::atomic<float> detection_rate{8.0f};
  std::atomic<int> debug_view{0};

  std::array<FaceResult, kMaxFaces> displayed_faces{};
  std::array<bool, kMaxFaces> face_present{};
  std::array<float, kMaxFaces> face_visibility{};
  uint64_t seen_generation = 0;
  uint64_t last_stage_time = 0;
  int detector_width = 0;
  int detector_height = 0;
  bool stage_pending = false;
};

void destroy_graphics(SmoothFilter *filter)
{
  if (filter->stage)
    gs_stagesurface_destroy(filter->stage);
  if (filter->detector_render)
    gs_texrender_destroy(filter->detector_render);
  if (filter->capture)
    gs_texrender_destroy(filter->capture);
  if (filter->processing)
    gs_texrender_destroy(filter->processing);
  if (filter->effect)
    gs_effect_destroy(filter->effect);
  filter->stage = nullptr;
  filter->detector_render = nullptr;
  filter->capture = nullptr;
  filter->processing = nullptr;
  filter->effect = nullptr;
}

void reset_detection_surfaces(SmoothFilter *filter, int width, int height)
{
  if (filter->detector_width == width && filter->detector_height == height && filter->stage &&
      filter->detector_render)
    return;

  if (filter->stage)
    gs_stagesurface_destroy(filter->stage);
  if (filter->detector_render)
    gs_texrender_destroy(filter->detector_render);

  filter->detector_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
  filter->stage = gs_stagesurface_create(static_cast<uint32_t>(width), static_cast<uint32_t>(height), GS_RGBA);
  filter->detector_width = width;
  filter->detector_height = height;
  filter->stage_pending = false;
}

void submit_staged_frame(SmoothFilter *filter)
{
  if (!filter->stage_pending || !filter->stage || !filter->detector || !filter->detector->ready())
    return;

  uint8_t *mapped = nullptr;
  uint32_t linesize = 0;
  if (!gs_stagesurface_map(filter->stage, &mapped, &linesize))
    return;

  const size_t row_bytes = static_cast<size_t>(filter->detector_width) * 4;
  std::vector<uint8_t> pixels(row_bytes * static_cast<size_t>(filter->detector_height));
  for (int row = 0; row < filter->detector_height; ++row) {
    std::memcpy(pixels.data() + static_cast<size_t>(row) * row_bytes,
                mapped + static_cast<size_t>(row) * linesize, row_bytes);
  }
  gs_stagesurface_unmap(filter->stage);
  filter->stage_pending = false;
  filter->detector->submit_rgba(std::move(pixels), filter->detector_width, filter->detector_height);
}

void draw_texture(gs_texture_t *texture, gs_effect_t *effect, uint32_t width, uint32_t height, const char *technique)
{
  gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
  gs_effect_set_texture(image, texture);
  while (gs_effect_loop(effect, technique))
    gs_draw_sprite(texture, 0, width, height);
}

gs_texture_t *render_texture(gs_texrender_t *destination, gs_texture_t *source, gs_effect_t *effect,
                             uint32_t width, uint32_t height, enum gs_color_space space, const char *technique)
{
  gs_texrender_reset(destination);
  gs_blend_state_push();
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
  if (!gs_texrender_begin_with_color_space(destination, width, height, space)) {
    gs_blend_state_pop();
    return nullptr;
  }

  struct vec4 clear;
  vec4_zero(&clear);
  gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
  gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
  draw_texture(source, effect, width, height, technique);
  gs_texrender_end(destination);
  gs_blend_state_pop();
  return gs_texrender_get_texture(destination);
}

void stage_detection_frame(SmoothFilter *filter, gs_texture_t *texture, uint32_t source_width, uint32_t source_height)
{
  if (!filter->detector || !filter->detector->ready() || filter->stage_pending)
    return;

  const uint64_t now = os_gettime_ns();
  const float rate = std::max(1.0f, filter->detection_rate.load());
  const uint64_t interval = static_cast<uint64_t>(1000000000.0 / rate);
  if (now - filter->last_stage_time < interval)
    return;
  filter->last_stage_time = now;

  constexpr int detection_width = 320;
  int detection_height = static_cast<int>(std::lround(detection_width *
                                      static_cast<double>(source_height) / static_cast<double>(source_width)));
  detection_height = std::clamp(detection_height, 96, 240);
  detection_height += detection_height % 2;
  reset_detection_surfaces(filter, detection_width, detection_height);
  if (!filter->detector_render || !filter->stage)
    return;

  gs_texrender_reset(filter->detector_render);
  if (!gs_texrender_begin_with_color_space(filter->detector_render, detection_width, detection_height, GS_CS_SRGB))
    return;

  struct vec4 clear;
  vec4_zero(&clear);
  gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
  gs_ortho(0.0f, static_cast<float>(detection_width), 0.0f, static_cast<float>(detection_height), -100.0f, 100.0f);
  draw_texture(texture, obs_get_base_effect(OBS_EFFECT_DEFAULT), detection_width, detection_height, "Draw");
  gs_texrender_end(filter->detector_render);

  gs_texture_t *small_texture = gs_texrender_get_texture(filter->detector_render);
  if (small_texture) {
    gs_stage_texture(filter->stage, small_texture);
    filter->stage_pending = true;
  }
}

void update_face_tracking(SmoothFilter *filter)
{
  const DetectionResult newest = filter->detector ? filter->detector->latest() : DetectionResult{};
  if (newest.generation != filter->seen_generation) {
    filter->seen_generation = newest.generation;
    for (size_t index = 0; index < kMaxFaces; ++index) {
      filter->face_present[index] = index < newest.count && newest.faces[index].confidence > 0.0f;
      if (filter->face_present[index])
        filter->displayed_faces[index] = newest.faces[index];
    }
  }

  // A short fade hides detector cadence without making the face region lag noticeably.
  for (size_t index = 0; index < kMaxFaces; ++index) {
    const float target = filter->face_present[index] ? 1.0f : 0.0f;
    filter->face_visibility[index] += (target - filter->face_visibility[index]) * 0.18f;
  }
}

const char *smooth_name(void *) { return obs_module_text("SmoothFilter"); }

void smooth_update(void *data, obs_data_t *settings)
{
  auto *filter = static_cast<SmoothFilter *>(data);
  filter->smooth_strength.store(static_cast<float>(obs_data_get_double(settings, kSmoothStrength)));
  filter->smooth_center.store(static_cast<float>(obs_data_get_double(settings, kSmoothCenter)));
  filter->smooth_radius.store(static_cast<float>(obs_data_get_double(settings, kSmoothRadius)));
  filter->smooth_chroma.store(static_cast<float>(obs_data_get_double(settings, kSmoothChroma)));
  filter->eye_strength.store(static_cast<float>(obs_data_get_double(settings, kEyeStrength)));
  filter->sharpen_position.store(static_cast<int>(obs_data_get_int(settings, kSharpenPosition)));
  filter->sharpen_amount.store(static_cast<float>(obs_data_get_double(settings, kSharpenAmount)));
  filter->sharpen_radius.store(static_cast<float>(obs_data_get_double(settings, kSharpenRadius)));
  filter->sharpen_chroma.store(static_cast<float>(obs_data_get_double(settings, kSharpenChroma)));
  filter->sharpen_scope.store(static_cast<int>(obs_data_get_int(settings, kSharpenScope)));
  filter->detection_rate.store(static_cast<float>(obs_data_get_double(settings, kDetectionRate)));
  filter->debug_view.store(static_cast<int>(obs_data_get_int(settings, kDebugView)));
}

void smooth_defaults(obs_data_t *settings)
{
  obs_data_set_default_double(settings, kSmoothStrength, 0.55);
  obs_data_set_default_double(settings, kSmoothCenter, 7.0);
  obs_data_set_default_double(settings, kSmoothRadius, 10.0);
  obs_data_set_default_double(settings, kSmoothChroma, 1.0);
  obs_data_set_default_double(settings, kEyeStrength, 0.45);
  obs_data_set_default_int(settings, kSharpenPosition, 0);
  obs_data_set_default_double(settings, kSharpenAmount, 0.35);
  obs_data_set_default_double(settings, kSharpenRadius, 1.0);
  obs_data_set_default_double(settings, kSharpenChroma, 0.0);
  obs_data_set_default_int(settings, kSharpenScope, 0);
  obs_data_set_default_double(settings, kDetectionRate, 8.0);
  obs_data_set_default_int(settings, kDebugView, 0);
}

obs_properties_t *smooth_properties(void *)
{
  obs_properties_t *properties = obs_properties_create();
  obs_property_t *amount = obs_properties_add_float_slider(properties, kSmoothStrength,
                                                            obs_module_text("SmoothStrength"), 0.0, 2.0, 0.01);
  obs_property_set_long_description(amount, obs_module_text("SmoothStrengthDescription"));
  obs_property_t *center = obs_properties_add_float_slider(properties, kSmoothCenter,
                                                            obs_module_text("SmoothCenter"), 1.5, 80.0, 0.5);
  obs_property_float_set_suffix(center, obs_module_text("SmoothRadiusSuffix"));
  obs_property_set_long_description(center, obs_module_text("SmoothCenterDescription"));
  obs_property_t *width = obs_properties_add_float_slider(properties, kSmoothRadius,
                                                           obs_module_text("SmoothRadius"), 1.0, 120.0, 0.5);
  obs_property_float_set_suffix(width, obs_module_text("SmoothRadiusSuffix"));
  obs_property_set_long_description(width, obs_module_text("SmoothRadiusDescription"));
  obs_property_t *smooth_chroma = obs_properties_add_float_slider(properties, kSmoothChroma,
                                                                   obs_module_text("SmoothChroma"), 0.0, 1.0, 0.01);
  obs_property_set_long_description(smooth_chroma, obs_module_text("SmoothChromaDescription"));
  obs_properties_add_float_slider(properties, kEyeStrength, obs_module_text("EyeStrength"), 0.0, 1.0, 0.01);
  obs_property_t *sharpen_position = obs_properties_add_list(properties, kSharpenPosition,
                                                              obs_module_text("SharpenPosition"),
                                                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(sharpen_position, obs_module_text("SharpenOff"), 0);
  obs_property_list_add_int(sharpen_position, obs_module_text("SharpenBefore"), 1);
  obs_property_list_add_int(sharpen_position, obs_module_text("SharpenAfter"), 2);
  obs_property_t *sharpen_scope = obs_properties_add_list(properties, kSharpenScope,
                                                           obs_module_text("SharpenScope"),
                                                           OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(sharpen_scope, obs_module_text("SharpenScopeFull"), 0);
  obs_property_list_add_int(sharpen_scope, obs_module_text("SharpenScopeFaces"), 1);
  obs_properties_add_float_slider(properties, kSharpenAmount, obs_module_text("SharpenAmount"), 0.0, 2.0, 0.01);
  obs_property_t *sharpen_radius = obs_properties_add_float_slider(properties, kSharpenRadius,
                                                                    obs_module_text("SharpenRadius"), 0.5, 8.0, 0.1);
  obs_property_float_set_suffix(sharpen_radius, obs_module_text("SmoothRadiusSuffix"));
  obs_property_t *sharpen_chroma = obs_properties_add_float_slider(properties, kSharpenChroma,
                                                                    obs_module_text("SharpenChroma"), 0.0, 1.0, 0.01);
  obs_property_set_long_description(sharpen_chroma, obs_module_text("SharpenChromaDescription"));
  obs_property_t *rate = obs_properties_add_float_slider(properties, kDetectionRate,
                                                          obs_module_text("DetectionRate"), 1.0, 15.0, 1.0);
  obs_property_float_set_suffix(rate, obs_module_text("DetectionRateSuffix"));
  obs_property_t *debug = obs_properties_add_list(properties, kDebugView, obs_module_text("DebugView"),
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(debug, obs_module_text("DebugViewOff"), 0);
  obs_property_list_add_int(debug, obs_module_text("DebugViewGeometry"), 1);
  obs_property_list_add_int(debug, obs_module_text("DebugViewTreatment"), 2);
  obs_property_list_add_int(debug, obs_module_text("DebugViewSharpen"), 3);
  return properties;
}

void *smooth_create(obs_data_t *settings, obs_source_t *source)
{
  auto filter = std::make_unique<SmoothFilter>();
  filter->source = source;

  char *effect_path = obs_module_file("effects/smooth.effect");
  char *model_path = obs_module_file("models/face_detection_yunet_2023mar.onnx");

  obs_enter_graphics();
  char *error = nullptr;
  filter->effect = effect_path ? gs_effect_create_from_file(effect_path, &error) : nullptr;
  if (filter->effect) {
    static const char *face_center_names[kMaxFaces] = {"face_center", "face_center_2", "face_center_3"};
    static const char *face_size_names[kMaxFaces] = {"face_size", "face_size_2", "face_size_3"};
    static const char *left_eye_names[kMaxFaces] = {"left_eye", "left_eye_2", "left_eye_3"};
    static const char *right_eye_names[kMaxFaces] = {"right_eye", "right_eye_2", "right_eye_3"};
    static const char *alpha_names[kMaxFaces] = {"detection_alpha", "detection_alpha_2", "detection_alpha_3"};
    filter->image_param = gs_effect_get_param_by_name(filter->effect, "image");
    filter->texel_size_param = gs_effect_get_param_by_name(filter->effect, "texel_size");
    for (size_t index = 0; index < kMaxFaces; ++index) {
      filter->face_center_params[index] = gs_effect_get_param_by_name(filter->effect, face_center_names[index]);
      filter->face_size_params[index] = gs_effect_get_param_by_name(filter->effect, face_size_names[index]);
      filter->left_eye_params[index] = gs_effect_get_param_by_name(filter->effect, left_eye_names[index]);
      filter->right_eye_params[index] = gs_effect_get_param_by_name(filter->effect, right_eye_names[index]);
      filter->detection_alpha_params[index] = gs_effect_get_param_by_name(filter->effect, alpha_names[index]);
    }
    filter->smooth_strength_param = gs_effect_get_param_by_name(filter->effect, "smooth_strength");
    filter->smooth_center_param = gs_effect_get_param_by_name(filter->effect, "smooth_center");
    filter->smooth_radius_param = gs_effect_get_param_by_name(filter->effect, "smooth_radius");
    filter->smooth_chroma_param = gs_effect_get_param_by_name(filter->effect, "smooth_chroma");
    filter->eye_strength_param = gs_effect_get_param_by_name(filter->effect, "eye_strength");
    filter->sharpen_amount_param = gs_effect_get_param_by_name(filter->effect, "sharpen_amount");
    filter->sharpen_radius_param = gs_effect_get_param_by_name(filter->effect, "sharpen_radius");
    filter->sharpen_chroma_param = gs_effect_get_param_by_name(filter->effect, "sharpen_chroma");
    filter->sharpen_masked_param = gs_effect_get_param_by_name(filter->effect, "sharpen_masked");
    filter->debug_view_param = gs_effect_get_param_by_name(filter->effect, "debug_view");
  }
  obs_leave_graphics();

  if (!filter->effect) {
    blog(LOG_ERROR, "[smooth] Could not load GPU effect: %s", error ? error : "unknown error");
    if (error)
      bfree(error);
    if (effect_path)
      bfree(effect_path);
    if (model_path)
      bfree(model_path);
    return nullptr;
  }
  if (error)
    bfree(error);

  if (model_path)
    filter->detector = std::make_unique<FaceDetector>(model_path);
  else
    blog(LOG_ERROR, "[smooth] YuNet model was not found");

  if (effect_path)
    bfree(effect_path);
  if (model_path)
    bfree(model_path);

  smooth_update(filter.get(), settings);
  return filter.release();
}

void smooth_destroy(void *data)
{
  auto *filter = static_cast<SmoothFilter *>(data);
  if (!filter)
    return;
  filter->detector.reset();
  obs_enter_graphics();
  destroy_graphics(filter);
  obs_leave_graphics();
  delete filter;
}

void set_face_parameters(SmoothFilter *filter, uint32_t width, uint32_t height)
{
  struct vec2 texel_size;
  vec2_set(&texel_size, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
  gs_effect_set_vec2(filter->texel_size_param, &texel_size);

  for (size_t index = 0; index < kMaxFaces; ++index) {
    const FaceResult &face = filter->displayed_faces[index];
    struct vec2 center, size, left_eye, right_eye;
    vec2_set(&center, face.center_x, face.center_y);
    vec2_set(&size, face.width, face.height);
    vec2_set(&left_eye, face.left_eye_x, face.left_eye_y);
    vec2_set(&right_eye, face.right_eye_x, face.right_eye_y);
    gs_effect_set_vec2(filter->face_center_params[index], &center);
    gs_effect_set_vec2(filter->face_size_params[index], &size);
    gs_effect_set_vec2(filter->left_eye_params[index], &left_eye);
    gs_effect_set_vec2(filter->right_eye_params[index], &right_eye);
    gs_effect_set_float(filter->detection_alpha_params[index],
                        std::clamp(filter->face_visibility[index], 0.0f, 1.0f));
  }
}

void set_beauty_parameters(SmoothFilter *filter, uint32_t width, uint32_t height, int debug_view)
{
  set_face_parameters(filter, width, height);
  gs_effect_set_float(filter->smooth_strength_param, filter->smooth_strength.load());
  gs_effect_set_float(filter->smooth_center_param, filter->smooth_center.load());
  gs_effect_set_float(filter->smooth_radius_param, filter->smooth_radius.load());
  gs_effect_set_float(filter->smooth_chroma_param, filter->smooth_chroma.load());
  gs_effect_set_float(filter->eye_strength_param, filter->eye_strength.load());
  gs_effect_set_float(filter->debug_view_param, static_cast<float>(debug_view));
}

void set_sharpen_parameters(SmoothFilter *filter, uint32_t width, uint32_t height)
{
  set_face_parameters(filter, width, height);
  gs_effect_set_float(filter->sharpen_amount_param, filter->sharpen_amount.load());
  gs_effect_set_float(filter->sharpen_radius_param, filter->sharpen_radius.load());
  gs_effect_set_float(filter->sharpen_chroma_param, filter->sharpen_chroma.load());
  gs_effect_set_float(filter->sharpen_masked_param, filter->sharpen_scope.load() == 1 ? 1.0f : 0.0f);
}

void smooth_render(void *data, gs_effect_t *)
{
  auto *filter = static_cast<SmoothFilter *>(data);
  obs_source_t *target = obs_filter_get_target(filter->source);
  obs_source_t *parent = obs_filter_get_parent(filter->source);
  if (!target || !parent || !filter->effect) {
    obs_source_skip_video_filter(filter->source);
    return;
  }

  const uint32_t width = obs_source_get_base_width(target);
  const uint32_t height = obs_source_get_base_height(target);
  if (!width || !height) {
    obs_source_skip_video_filter(filter->source);
    return;
  }

  const enum gs_color_space preferred[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
  const enum gs_color_space space = obs_source_get_color_space(target, 3, preferred);
  const enum gs_color_format format = gs_get_format_from_space(space);
  if (!filter->capture || !filter->processing || filter->capture_format != format) {
    if (filter->capture)
      gs_texrender_destroy(filter->capture);
    if (filter->processing)
      gs_texrender_destroy(filter->processing);
    filter->capture = gs_texrender_create(format, GS_ZS_NONE);
    filter->processing = gs_texrender_create(format, GS_ZS_NONE);
    filter->capture_format = format;
  }
  if (!filter->capture || !filter->processing) {
    obs_source_skip_video_filter(filter->source);
    return;
  }

  gs_texrender_reset(filter->capture);
  gs_blend_state_push();
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
  if (gs_texrender_begin_with_color_space(filter->capture, width, height, space)) {
    struct vec4 clear;
    vec4_zero(&clear);
    gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
    gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);

    const uint32_t target_flags = obs_source_get_output_flags(target);
    const bool custom_draw = (target_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
    const bool async = (target_flags & OBS_SOURCE_ASYNC) != 0;
    if (target == parent && !custom_draw && !async)
      obs_source_default_render(target);
    else
      obs_source_video_render(target);
    gs_texrender_end(filter->capture);
  }
  gs_blend_state_pop();

  gs_texture_t *texture = gs_texrender_get_texture(filter->capture);
  if (!texture)
    return;

  submit_staged_frame(filter);
  stage_detection_frame(filter, texture, width, height);
  update_face_tracking(filter);

  const float maximum_visibility = *std::max_element(filter->face_visibility.begin(), filter->face_visibility.end());
  const int debug_view = filter->debug_view.load();
  const int sharpen_position = filter->sharpen_position.load();
  if (maximum_visibility < 0.001f && debug_view == 0 && sharpen_position == 0) {
    draw_texture(texture, obs_get_base_effect(OBS_EFFECT_DEFAULT), width, height, "Draw");
    return;
  }

  if (debug_view != 0 || sharpen_position == 0) {
    set_beauty_parameters(filter, width, height, debug_view);
    draw_texture(texture, filter->effect, width, height, "Draw");
    return;
  }

  if (sharpen_position == 1) {
    set_sharpen_parameters(filter, width, height);
    gs_texture_t *sharpened = render_texture(filter->processing, texture, filter->effect,
                                             width, height, space, "Sharpen");
    set_beauty_parameters(filter, width, height, debug_view);
    if (sharpened)
      draw_texture(sharpened, filter->effect, width, height, "Draw");
    else
      draw_texture(texture, filter->effect, width, height, "Draw");
    return;
  }

  set_beauty_parameters(filter, width, height, debug_view);
  gs_texture_t *smoothed = render_texture(filter->processing, texture, filter->effect,
                                          width, height, space, "Draw");
  if (smoothed) {
    set_sharpen_parameters(filter, width, height);
    draw_texture(smoothed, filter->effect, width, height, "Sharpen");
  } else {
    set_beauty_parameters(filter, width, height, debug_view);
    draw_texture(texture, filter->effect, width, height, "Draw");
  }
}

enum gs_color_space smooth_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
  auto *filter = static_cast<SmoothFilter *>(data);
  obs_source_t *target = obs_filter_get_target(filter->source);
  return target ? obs_source_get_color_space(target, count, preferred_spaces)
                : (count > 0 ? preferred_spaces[0] : GS_CS_SRGB);
}
} // namespace

MODULE_EXPORT const char *obs_module_description(void)
{
  return "GPU face-aware smoothing and dark-eye correction";
}

bool obs_module_load(void)
{
  static obs_source_info info = {};
  info.id = "smooth_filter";
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
  info.get_name = smooth_name;
  info.create = smooth_create;
  info.destroy = smooth_destroy;
  info.update = smooth_update;
  info.get_defaults = smooth_defaults;
  info.get_properties = smooth_properties;
  info.video_render = smooth_render;
  info.video_get_color_space = smooth_color_space;
  obs_register_source(&info);
  blog(LOG_INFO, "[smooth] Loaded version %s", SMOOTH_VERSION);
  return true;
}
