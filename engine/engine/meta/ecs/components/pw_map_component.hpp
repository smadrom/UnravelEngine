#pragma once

#include <engine/pw/pw_map_component.h>
#include <reflection/reflection.h>
#include <serialization/serialization.h>

namespace unravel
{
SAVE_EXTERN(pw_map_component);
LOAD_EXTERN(pw_map_component);
REFLECT_EXTERN(pw_map_component);
SAVE_EXTERN(pw_map_generated_component);
LOAD_EXTERN(pw_map_generated_component);
REFLECT_EXTERN(pw_map_generated_component);
} // namespace unravel
