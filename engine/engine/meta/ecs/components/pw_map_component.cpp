#include "pw_map_component.hpp"

#include <engine/meta/core/common/basetypes.hpp>
#include <serialization/associative_archive.h>
#include <serialization/binary_archive.h>
#include <serialization/types/utility.hpp>

namespace unravel
{
REFLECT(pw_map_component)
{
    entt::meta_factory<pw_map_component>{}
        .type("pw_map_component"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "pw_map_component"},
            entt::attribute{"category", "PW"},
            entt::attribute{"pretty_name", "PW Map"},
        })
        .func<&component_meta<pw_map_component>::exists>("component_exists"_hs)
        .func<&component_meta<pw_map_component>::add>("component_add"_hs)
        .func<&component_meta<pw_map_component>::remove>("component_remove"_hs)
        .func<&component_meta<pw_map_component>::save>("component_save"_hs)
        .func<&component_meta<pw_map_component>::load>("component_load"_hs)
        .data<&pw_map_component::content_root>("content_root"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "content_root"},
            entt::attribute{"pretty_name", "Content Root"},
        })
        .data<&pw_map_component::map_slug>("map_slug"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "map_slug"},
            entt::attribute{"pretty_name", "Map Slug"},
        })
        .data<&pw_map_component::auto_load>("auto_load"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "auto_load"},
            entt::attribute{"pretty_name", "Auto Load"},
        })
        .data<&pw_map_component::require_full>("require_full"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "require_full"},
            entt::attribute{"pretty_name", "Require Full Map"},
        })
        .data<&pw_map_component::buildings_per_frame>("buildings_per_frame"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "buildings_per_frame"},
            entt::attribute{"pretty_name", "Buildings Per Frame"},
            entt::attribute{"min", uint32_t{1}},
        })
        .data<nullptr, &pw_map_component::status>("status"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "status"},
            entt::attribute{"pretty_name", "Status"},
        })
        .data<nullptr, &pw_map_component::error>("error"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "error"},
            entt::attribute{"pretty_name", "Error"},
        });
}

SAVE(pw_map_component)
{
    try_save(ar, ser20::make_nvp("content_root", obj.content_root));
    try_save(ar, ser20::make_nvp("map_slug", obj.map_slug));
    try_save(ar, ser20::make_nvp("auto_load", obj.auto_load));
    try_save(ar, ser20::make_nvp("require_full", obj.require_full));
    try_save(ar, ser20::make_nvp("buildings_per_frame", obj.buildings_per_frame));
}
SAVE_INSTANTIATE(pw_map_component, ser20::oarchive_associative_t);
SAVE_INSTANTIATE(pw_map_component, ser20::oarchive_binary_t);

LOAD(pw_map_component)
{
    try_load(ar, ser20::make_nvp("content_root", obj.content_root));
    try_load(ar, ser20::make_nvp("map_slug", obj.map_slug));
    try_load(ar, ser20::make_nvp("auto_load", obj.auto_load));
    try_load(ar, ser20::make_nvp("require_full", obj.require_full));
    try_load(ar, ser20::make_nvp("buildings_per_frame", obj.buildings_per_frame));
    obj.status = "idle";
    obj.error.clear();
    obj.runtime_instance_token = 0;
}
LOAD_INSTANTIATE(pw_map_component, ser20::iarchive_associative_t);
LOAD_INSTANTIATE(pw_map_component, ser20::iarchive_binary_t);

REFLECT(pw_map_generated_component)
{
    entt::meta_factory<pw_map_generated_component>{}
        .type("pw_map_generated_component"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "pw_map_generated_component"},
            entt::attribute{"category", "PW"},
            entt::attribute{"pretty_name", "PW Generated Map"},
        })
        .func<&component_meta<pw_map_generated_component>::exists>("component_exists"_hs)
        .func<&component_meta<pw_map_generated_component>::add>("component_add"_hs)
        .func<&component_meta<pw_map_generated_component>::remove>("component_remove"_hs)
        .func<&component_meta<pw_map_generated_component>::save>("component_save"_hs)
        .func<&component_meta<pw_map_generated_component>::load>("component_load"_hs)
        .data<nullptr, &pw_map_generated_component::descriptor_id>("descriptor_id"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "descriptor_id"},
        })
        .data<nullptr, &pw_map_generated_component::generation>("generation"_hs)
        .custom<entt::attributes>(entt::attributes{
            entt::attribute{"name", "generation"},
        });
}

SAVE(pw_map_generated_component)
{
    try_save(ar, ser20::make_nvp("descriptor_id", obj.descriptor_id));
    try_save(ar, ser20::make_nvp("generation", obj.generation));
    try_save(ar, ser20::make_nvp("authored_environment_active", obj.authored_environment_active));
}
SAVE_INSTANTIATE(pw_map_generated_component, ser20::oarchive_associative_t);
SAVE_INSTANTIATE(pw_map_generated_component, ser20::oarchive_binary_t);

LOAD(pw_map_generated_component)
{
    try_load(ar, ser20::make_nvp("descriptor_id", obj.descriptor_id));
    try_load(ar, ser20::make_nvp("generation", obj.generation));
    try_load(ar, ser20::make_nvp("authored_environment_active", obj.authored_environment_active));
}
LOAD_INSTANTIATE(pw_map_generated_component, ser20::iarchive_associative_t);
LOAD_INSTANTIATE(pw_map_generated_component, ser20::iarchive_binary_t);

} // namespace unravel
