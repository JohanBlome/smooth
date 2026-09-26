#include <graphics/graphics.h>
#include <util/bmem.h>

#include <cstdio>

int main(int argc, char **argv)
{
  if (argc != 3) {
    std::fprintf(stderr, "usage: effect_smoke <graphics-module> <effect>\n");
    return 2;
  }
  graphics_t *graphics = nullptr;
  if (gs_create(&graphics, argv[1], 0) != GS_SUCCESS)
    return 4;

  char *error = nullptr;
  gs_enter_context(graphics);
  gs_effect_t *effect = gs_effect_create_from_file(argv[2], &error);
  std::fprintf(stderr, "effect compile: %s\n", effect ? "ok" : "failed");
  if (effect)
    gs_effect_destroy(effect);
  gs_leave_context();

  if (!effect)
    std::fprintf(stderr, "%s\n", error ? error : "effect compilation failed");
  if (error)
    bfree(error);
  gs_destroy(graphics);
  return effect ? 0 : 5;
}
