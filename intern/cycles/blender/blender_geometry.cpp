
/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "render/curves.h"
#include "render/hair.h"
#include "render/mesh.h"
#include "render/object.h"
#include "render/volume.h"

#include "blender/blender_sync.h"
#include "blender/blender_util.h"

#include "util/util_foreach.h"
#include "util/util_task.h"

CCL_NAMESPACE_BEGIN

static Geometry::Type determine_geom_type(BObjectInfo &b_ob_info, bool use_particle_hair)
{
  if (b_ob_info.object_data.is_a(&RNA_Hair) || use_particle_hair) {
    return Geometry::HAIR;
  }

  if (b_ob_info.object_data.is_a(&RNA_Volume) ||
      (b_ob_info.object_data == b_ob_info.real_object.data() &&
       object_fluid_gas_domain_find(b_ob_info.real_object))) {
    return Geometry::VOLUME;
  }

  return Geometry::MESH;
}

array<Node *> BlenderSync::find_used_shaders(BL::Object &b_ob)
{
  BL::Material material_override = view_layer.material_override;
  Shader *default_shader = (b_ob.type() == BL::Object::type_VOLUME) ? scene->default_volume :
                                                                      scene->default_surface;

  array<Node *> used_shaders;

  for (BL::MaterialSlot &b_slot : b_ob.material_slots) {
    if (material_override) {
      find_shader(material_override, used_shaders, default_shader);
    }
    else {
      BL::ID b_material(b_slot.material());
      find_shader(b_material, used_shaders, default_shader);
    }
  }

  if (used_shaders.size() == 0) {
    if (material_override)
      find_shader(material_override, used_shaders, default_shader);
    else
      used_shaders.push_back_slow(default_shader);
  }

  return used_shaders;
}

Geometry *BlenderSync::sync_geometry(BL::Depsgraph &b_depsgraph,
                                     BObjectInfo &b_ob_info,
                                     bool object_updated,
                                     bool use_particle_hair,
                                     TaskPool *task_pool)
{
  /* Test if we can instance or if the object is modified. */
  Geometry::Type geom_type = determine_geom_type(b_ob_info, use_particle_hair);
  GeometryKey key(b_ob_info.object_data, geom_type);

  /* Find shader indices. */
  array<Node *> used_shaders = find_used_shaders(b_ob_info.iter_object);

  /* Ensure we only sync instanced geometry once. */
  Geometry *geom = geometry_map.find(key);
  if (geom) {
    if (geometry_synced.find(geom) != geometry_synced.end()) {
      return geom;
    }
  }

  /* Test if we need to sync. */
  bool sync = true;
  if (geom == NULL) {
    /* Add new geometry if it did not exist yet. */
    if (geom_type == Geometry::HAIR) {
      geom = scene->create_node<Hair>();
    }
    else if (geom_type == Geometry::VOLUME) {
      geom = scene->create_node<Volume>();
    }
    else {
      geom = scene->create_node<Mesh>();
    }
    geometry_map.add(key, geom);
  }
  else {
    /* Test if we need to update existing geometry. */
    sync = geometry_map.update(geom, b_ob_info.object_data);
  }

  if (!sync) {
    /* If transform was applied to geometry, need full update. */
    if (object_updated && geom->transform_applied) {
      ;
    }
    /* Test if shaders changed, these can be object level so geometry
     * does not get tagged for recalc. */
    else if (geom->get_used_shaders() != used_shaders) {
      ;
    }
    else {
      /* Even if not tagged for recalc, we may need to sync anyway
       * because the shader needs different geometry attributes. */
      bool attribute_recalc = false;

      foreach (Node *node, geom->get_used_shaders()) {
        Shader *shader = static_cast<Shader *>(node);
        if (shader->need_update_geometry()) {
          attribute_recalc = true;
        }
      }

      if (!attribute_recalc) {
        return geom;
      }
    }
  }

  geometry_synced.insert(geom);

  geom->name = ustring(b_ob_info.object_data.name().c_str());

  /* Store the shaders immediately for the object attribute code. */
  geom->set_used_shaders(used_shaders);

  auto sync_func = [=]() mutable {
    if (progress.get_cancel())
      return;

    progress.set_sync_status("Synchronizing object", b_ob_info.real_object.name());

    if (geom_type == Geometry::HAIR) {
      Hair *hair = static_cast<Hair *>(geom);
      sync_hair(b_depsgraph, b_ob_info, hair);
    }
    else if (geom_type == Geometry::VOLUME) {
      Volume *volume = static_cast<Volume *>(geom);
      sync_volume(b_ob_info, volume);
    }
    else {
      Mesh *mesh = static_cast<Mesh *>(geom);
      sync_mesh(b_depsgraph, b_ob_info, mesh);
    }
  };

  /* Defer the actual geometry sync to the task_pool for multithreading */
  if (task_pool) {
    task_pool->push(sync_func);
  }
  else {
    sync_func();
  }

  return geom;
}

void BlenderSync::sync_geometry_motion(BL::Depsgraph &b_depsgraph,
                                       BObjectInfo &b_ob_info,
                                       Object *object,
                                       float motion_time,
                                       bool use_particle_hair,
                                       TaskPool *task_pool)
{
  /* Ensure we only sync instanced geometry once. */
  Geometry *geom = object->get_geometry();

  if (geometry_motion_synced.find(geom) != geometry_motion_synced.end())
    return;

  geometry_motion_synced.insert(geom);

  /* Ensure we only motion sync geometry that also had geometry synced, to avoid
   * unnecessary work and to ensure that its attributes were clear. */
  if (geometry_synced.find(geom) == geometry_synced.end())
    return;

  /* Find time matching motion step required by geometry. */
  int motion_step = geom->motion_step(motion_time);
  if (motion_step < 0) {
    return;
  }

  auto sync_func = [=]() mutable {
    if (progress.get_cancel())
      return;

    if (b_ob_info.object_data.is_a(&RNA_Hair) || use_particle_hair) {
      Hair *hair = static_cast<Hair *>(geom);
      sync_hair_motion(b_depsgraph, b_ob_info, hair, motion_step);
    }
    else if (b_ob_info.object_data.is_a(&RNA_Volume) ||
             object_fluid_gas_domain_find(b_ob_info.real_object)) {
      /* No volume motion blur support yet. */
    }
    else {
      Mesh *mesh = static_cast<Mesh *>(geom);
      sync_mesh_motion(b_depsgraph, b_ob_info, mesh, motion_step);
    }
  };

  /* Defer the actual geometry sync to the task_pool for multithreading */
  if (task_pool) {
    task_pool->push(sync_func);
  }
  else {
    sync_func();
  }
}

CCL_NAMESPACE_END
