/*************************************************************************/
/*  mesh_storage.cpp                                                     */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#ifdef GLES3_ENABLED

#include "mesh_storage.h"
#include "material_storage.h"
#include "utilities.h"

using namespace GLES3;

MeshStorage *MeshStorage::singleton = nullptr;

MeshStorage *MeshStorage::get_singleton() {
	return singleton;
}

MeshStorage::MeshStorage() {
	singleton = this;

	{
		skeleton_shader.shader.initialize();
		skeleton_shader.shader_version = skeleton_shader.shader.version_create();
	}
}

MeshStorage::~MeshStorage() {
	singleton = nullptr;
	skeleton_shader.shader.version_free(skeleton_shader.shader_version);
}

/* MESH API */

RID MeshStorage::mesh_allocate() {
	return mesh_owner.allocate_rid();
}

void MeshStorage::mesh_initialize(RID p_rid) {
	mesh_owner.initialize_rid(p_rid, Mesh());
}

void MeshStorage::mesh_free(RID p_rid) {
	mesh_clear(p_rid);
	mesh_set_shadow_mesh(p_rid, RID());
	Mesh *mesh = mesh_owner.get_or_null(p_rid);
	ERR_FAIL_COND(!mesh);

	mesh->dependency.deleted_notify(p_rid);
	if (mesh->instances.size()) {
		ERR_PRINT("deleting mesh with active instances");
	}
	if (mesh->shadow_owners.size()) {
		for (Mesh *E : mesh->shadow_owners) {
			Mesh *shadow_owner = E;
			shadow_owner->shadow_mesh = RID();
			shadow_owner->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);
		}
	}
	mesh_owner.free(p_rid);
}

void MeshStorage::mesh_set_blend_shape_count(RID p_mesh, int p_blend_shape_count) {
	ERR_FAIL_COND(p_blend_shape_count < 0);

	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND(!mesh);

	ERR_FAIL_COND(mesh->surface_count > 0); //surfaces already exist
	mesh->blend_shape_count = p_blend_shape_count;
}

bool MeshStorage::mesh_needs_instance(RID p_mesh, bool p_has_skeleton) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND_V(!mesh, false);

	return mesh->blend_shape_count > 0 || (mesh->has_bone_weights && p_has_skeleton);
}

void MeshStorage::mesh_add_surface(RID p_mesh, const RS::SurfaceData &p_surface) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND(!mesh);

	ERR_FAIL_COND(mesh->surface_count == RS::MAX_MESH_SURFACES);

#ifdef DEBUG_ENABLED
	//do a validation, to catch errors first
	{
		uint32_t stride = 0;
		uint32_t attrib_stride = 0;
		uint32_t skin_stride = 0;

		for (int i = 0; i < RS::ARRAY_WEIGHTS; i++) {
			if ((p_surface.format & (1 << i))) {
				switch (i) {
					case RS::ARRAY_VERTEX: {
						if (p_surface.format & RS::ARRAY_FLAG_USE_2D_VERTICES) {
							stride += sizeof(float) * 2;
						} else {
							stride += sizeof(float) * 3;
						}

					} break;
					case RS::ARRAY_NORMAL: {
						stride += sizeof(uint16_t) * 2;

					} break;
					case RS::ARRAY_TANGENT: {
						stride += sizeof(uint16_t) * 2;

					} break;
					case RS::ARRAY_COLOR: {
						attrib_stride += sizeof(uint32_t);
					} break;
					case RS::ARRAY_TEX_UV: {
						attrib_stride += sizeof(float) * 2;

					} break;
					case RS::ARRAY_TEX_UV2: {
						attrib_stride += sizeof(float) * 2;

					} break;
					case RS::ARRAY_CUSTOM0:
					case RS::ARRAY_CUSTOM1:
					case RS::ARRAY_CUSTOM2:
					case RS::ARRAY_CUSTOM3: {
						int idx = i - RS::ARRAY_CUSTOM0;
						uint32_t fmt_shift[RS::ARRAY_CUSTOM_COUNT] = { RS::ARRAY_FORMAT_CUSTOM0_SHIFT, RS::ARRAY_FORMAT_CUSTOM1_SHIFT, RS::ARRAY_FORMAT_CUSTOM2_SHIFT, RS::ARRAY_FORMAT_CUSTOM3_SHIFT };
						uint32_t fmt = (p_surface.format >> fmt_shift[idx]) & RS::ARRAY_FORMAT_CUSTOM_MASK;
						uint32_t fmtsize[RS::ARRAY_CUSTOM_MAX] = { 4, 4, 4, 8, 4, 8, 12, 16 };
						attrib_stride += fmtsize[fmt];

					} break;
					case RS::ARRAY_WEIGHTS:
					case RS::ARRAY_BONES: {
						//uses a separate array
						bool use_8 = p_surface.format & RS::ARRAY_FLAG_USE_8_BONE_WEIGHTS;
						skin_stride += sizeof(int16_t) * (use_8 ? 16 : 8);
					} break;
				}
			}
		}

		int expected_size = stride * p_surface.vertex_count;
		ERR_FAIL_COND_MSG(expected_size != p_surface.vertex_data.size(), "Size of vertex data provided (" + itos(p_surface.vertex_data.size()) + ") does not match expected (" + itos(expected_size) + ")");

		int bs_expected_size = expected_size * mesh->blend_shape_count;

		ERR_FAIL_COND_MSG(bs_expected_size != p_surface.blend_shape_data.size(), "Size of blend shape data provided (" + itos(p_surface.blend_shape_data.size()) + ") does not match expected (" + itos(bs_expected_size) + ")");

		int expected_attrib_size = attrib_stride * p_surface.vertex_count;
		ERR_FAIL_COND_MSG(expected_attrib_size != p_surface.attribute_data.size(), "Size of attribute data provided (" + itos(p_surface.attribute_data.size()) + ") does not match expected (" + itos(expected_attrib_size) + ")");

		if ((p_surface.format & RS::ARRAY_FORMAT_WEIGHTS) && (p_surface.format & RS::ARRAY_FORMAT_BONES)) {
			expected_size = skin_stride * p_surface.vertex_count;
			ERR_FAIL_COND_MSG(expected_size != p_surface.skin_data.size(), "Size of skin data provided (" + itos(p_surface.skin_data.size()) + ") does not match expected (" + itos(expected_size) + ")");
		}
	}

#endif

	Mesh::Surface *s = memnew(Mesh::Surface);

	s->format = p_surface.format;
	s->primitive = p_surface.primitive;

	if (p_surface.vertex_data.size()) {
		glGenBuffers(1, &s->vertex_buffer);
		glBindBuffer(GL_ARRAY_BUFFER, s->vertex_buffer);
		glBufferData(GL_ARRAY_BUFFER, p_surface.vertex_data.size(), p_surface.vertex_data.ptr(), (s->format & RS::ARRAY_FLAG_USE_DYNAMIC_UPDATE) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0); //unbind
		s->vertex_buffer_size = p_surface.vertex_data.size();
	}

	if (p_surface.attribute_data.size()) {
		glGenBuffers(1, &s->attribute_buffer);
		glBindBuffer(GL_ARRAY_BUFFER, s->attribute_buffer);
		glBufferData(GL_ARRAY_BUFFER, p_surface.attribute_data.size(), p_surface.attribute_data.ptr(), (s->format & RS::ARRAY_FLAG_USE_DYNAMIC_UPDATE) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0); //unbind
		s->attribute_buffer_size = p_surface.attribute_data.size();
	}
	if (p_surface.skin_data.size()) {
		glGenBuffers(1, &s->skin_buffer);
		glBindBuffer(GL_ARRAY_BUFFER, s->skin_buffer);
		glBufferData(GL_ARRAY_BUFFER, p_surface.skin_data.size(), p_surface.skin_data.ptr(), (s->format & RS::ARRAY_FLAG_USE_DYNAMIC_UPDATE) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0); //unbind
		s->skin_buffer_size = p_surface.skin_data.size();
	}

	s->vertex_count = p_surface.vertex_count;

	if (p_surface.format & RS::ARRAY_FORMAT_BONES) {
		mesh->has_bone_weights = true;
	}

	if (p_surface.index_count) {
		bool is_index_16 = p_surface.vertex_count <= 65536 && p_surface.vertex_count > 0;
		glGenBuffers(1, &s->index_buffer);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->index_buffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, p_surface.index_data.size(), p_surface.index_data.ptr(), GL_STATIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); //unbind
		s->index_count = p_surface.index_count;
		s->index_buffer_size = p_surface.index_data.size();

		if (p_surface.lods.size()) {
			s->lods = memnew_arr(Mesh::Surface::LOD, p_surface.lods.size());
			s->lod_count = p_surface.lods.size();

			for (int i = 0; i < p_surface.lods.size(); i++) {
				glGenBuffers(1, &s->lods[i].index_buffer);
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->lods[i].index_buffer);
				glBufferData(GL_ELEMENT_ARRAY_BUFFER, p_surface.lods[i].index_data.size(), p_surface.lods[i].index_data.ptr(), GL_STATIC_DRAW);
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); //unbind
				s->lods[i].edge_length = p_surface.lods[i].edge_length;
				s->lods[i].index_count = p_surface.lods[i].index_data.size() / (is_index_16 ? 2 : 4);
				s->lods[i].index_buffer_size = p_surface.lods[i].index_data.size();
			}
		}
	}

	ERR_FAIL_COND_MSG(!p_surface.index_count && !p_surface.vertex_count, "Meshes must contain a vertex array, an index array, or both");

	s->aabb = p_surface.aabb;
	s->bone_aabbs = p_surface.bone_aabbs; //only really useful for returning them.

	if (p_surface.skin_data.size() || mesh->blend_shape_count > 0) {
		// Size must match the size of the vertex array.
		int size = p_surface.vertex_data.size();
		int vertex_size = 0;
		int stride = 0;
		int normal_offset = 0;
		int tangent_offset = 0;
		if ((p_surface.format & (1 << RS::ARRAY_VERTEX))) {
			if (p_surface.format & RS::ARRAY_FLAG_USE_2D_VERTICES) {
				vertex_size = 2;
			} else {
				vertex_size = 3;
			}
			stride = sizeof(float) * vertex_size;
		}
		if ((p_surface.format & (1 << RS::ARRAY_NORMAL))) {
			normal_offset = stride;
			stride += sizeof(uint16_t) * 2;
		}
		if ((p_surface.format & (1 << RS::ARRAY_TANGENT))) {
			tangent_offset = stride;
			stride += sizeof(uint16_t) * 2;
		}

		if (mesh->blend_shape_count > 0) {
			// Blend shapes are passed as one large array, for OpenGL, we need to split each of them into their own buffer
			s->blend_shapes = memnew_arr(Mesh::Surface::BlendShape, mesh->blend_shape_count);

			for (uint32_t i = 0; i < mesh->blend_shape_count; i++) {
				glGenVertexArrays(1, &s->blend_shapes[i].vertex_array);
				glBindVertexArray(s->blend_shapes[i].vertex_array);
				glGenBuffers(1, &s->blend_shapes[i].vertex_buffer);
				glBindBuffer(GL_ARRAY_BUFFER, s->blend_shapes[i].vertex_buffer);
				glBufferData(GL_ARRAY_BUFFER, size, p_surface.blend_shape_data.ptr() + i * size, (s->format & RS::ARRAY_FLAG_USE_DYNAMIC_UPDATE) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);

				if ((p_surface.format & (1 << RS::ARRAY_VERTEX))) {
					glEnableVertexAttribArray(RS::ARRAY_VERTEX + 3);
					glVertexAttribPointer(RS::ARRAY_VERTEX + 3, vertex_size, GL_FLOAT, GL_FALSE, stride, CAST_INT_TO_UCHAR_PTR(0));
				}
				if ((p_surface.format & (1 << RS::ARRAY_NORMAL))) {
					glEnableVertexAttribArray(RS::ARRAY_NORMAL + 3);
					glVertexAttribPointer(RS::ARRAY_NORMAL + 3, 2, GL_UNSIGNED_SHORT, GL_TRUE, stride, CAST_INT_TO_UCHAR_PTR(normal_offset));
				}
				if ((p_surface.format & (1 << RS::ARRAY_TANGENT))) {
					glEnableVertexAttribArray(RS::ARRAY_TANGENT + 3);
					glVertexAttribPointer(RS::ARRAY_TANGENT + 3, 2, GL_UNSIGNED_SHORT, GL_TRUE, stride, CAST_INT_TO_UCHAR_PTR(tangent_offset));
				}
			}
			glBindVertexArray(0);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
		}

		// Create a vertex array to use for skeleton/blend shapes.
		glGenVertexArrays(1, &s->skeleton_vertex_array);
		glBindVertexArray(s->skeleton_vertex_array);
		glBindBuffer(GL_ARRAY_BUFFER, s->vertex_buffer);

		if ((p_surface.format & (1 << RS::ARRAY_VERTEX))) {
			glEnableVertexAttribArray(RS::ARRAY_VERTEX);
			glVertexAttribPointer(RS::ARRAY_VERTEX, vertex_size, GL_FLOAT, GL_FALSE, stride, CAST_INT_TO_UCHAR_PTR(0));
		}
		if ((p_surface.format & (1 << RS::ARRAY_NORMAL))) {
			glEnableVertexAttribArray(RS::ARRAY_NORMAL);
			glVertexAttribPointer(RS::ARRAY_NORMAL, 2, GL_UNSIGNED_SHORT, GL_TRUE, stride, CAST_INT_TO_UCHAR_PTR(normal_offset));
		}
		if ((p_surface.format & (1 << RS::ARRAY_TANGENT))) {
			glEnableVertexAttribArray(RS::ARRAY_TANGENT);
			glVertexAttribPointer(RS::ARRAY_TANGENT, 2, GL_UNSIGNED_SHORT, GL_TRUE, stride, CAST_INT_TO_UCHAR_PTR(tangent_offset));
		}
		glBindVertexArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	if (mesh->surface_count == 0) {
		mesh->bone_aabbs = p_surface.bone_aabbs;
		mesh->aabb = p_surface.aabb;
	} else {
		if (mesh->bone_aabbs.size() < p_surface.bone_aabbs.size()) {
			// ArrayMesh::_surface_set_data only allocates bone_aabbs up to max_bone
			// Each surface may affect different numbers of bones.
			mesh->bone_aabbs.resize(p_surface.bone_aabbs.size());
		}
		for (int i = 0; i < p_surface.bone_aabbs.size(); i++) {
			const AABB &bone = p_surface.bone_aabbs[i];
			if (bone.has_volume()) {
				AABB &mesh_bone = mesh->bone_aabbs.write[i];
				if (mesh_bone != AABB()) {
					// Already initialized, merge AABBs.
					mesh_bone.merge_with(bone);
				} else {
					// Not yet initialized, copy the bone AABB.
					mesh_bone = bone;
				}
			}
		}
		mesh->aabb.merge_with(p_surface.aabb);
	}

	s->material = p_surface.material;

	mesh->surfaces = (Mesh::Surface **)memrealloc(mesh->surfaces, sizeof(Mesh::Surface *) * (mesh->surface_count + 1));
	mesh->surfaces[mesh->surface_count] = s;
	mesh->surface_count++;

	for (MeshInstance *mi : mesh->instances) {
		_mesh_instance_add_surface(mi, mesh, mesh->surface_count - 1);
	}

	mesh->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);

	for (Mesh *E : mesh->shadow_owners) {
		Mesh *shadow_owner = E;
		shadow_owner->shadow_mesh = RID();
		shadow_owner->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);
	}

	mesh->material_cache.clear();
}

int MeshStorage::mesh_get_blend_shape_count(RID p_mesh) const {
	const Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND_V(!mesh, -1);
	return mesh->blend_shape_count;
}

void MeshStorage::mesh_set_blend_shape_mode(RID p_mesh, RS::BlendShapeMode p_mode) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND(!mesh);
	ERR_FAIL_INDEX((int)p_mode, 2);

	mesh->blend_shape_mode = p_mode;
}

RS::BlendShapeMode MeshStorage::mesh_get_blend_shape_mode(RID p_mesh) const {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND_V(!mesh, RS::BLEND_SHAPE_MODE_NORMALIZED);
	return mesh->blend_shape_mode;
}

void MeshStorage::mesh_surface_update_vertex_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND(!mesh);
	ERR_FAIL_UNSIGNED_INDEX((uint32_t)p_surface, mesh->surface_count);
	ERR_FAIL_COND(p_data.size() == 0);

	uint64_t data_size = p_data.size();
	ERR_FAIL_COND(p_offset + data_size > mesh->surfaces[p_surface]->vertex_buffer_size);
	const uint8_t *r = p_data.ptr();

	glBindBuffer(GL_ARRAY_BUFFER, mesh->surfaces[p_surface]->vertex_buffer);
	glBufferSubData(GL_ARRAY_BUFFER, p_offset, data_size, r);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void MeshStorage::mesh_surface_update_attribute_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND(!mesh);
	ERR_FAIL_UNSIGNED_INDEX((uint32_t)p_surface, mesh->surface_count);
	ERR_FAIL_COND(p_data.size() == 0);

	uint64_t data_size = p_data.size();
	ERR_FAIL_COND(p_offset + data_size > mesh->surfaces[p_surface]->attribute_buffer_size);
	const uint8_t *r = p_data.ptr();

	glBindBuffer(GL_ARRAY_BUFFER, mesh->surfaces[p_surface]->attribute_buffer);
	glBufferSubData(GL_ARRAY_BUFFER, p_offset, data_size, r);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void MeshStorage::mesh_surface_update_skin_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND(!mesh);
	ERR_FAIL_UNSIGNED_INDEX((uint32_t)p_surface, mesh->surface_count);
	ERR_FAIL_COND(p_data.size() == 0);

	uint64_t data_size = p_data.size();
	ERR_FAIL_COND(p_offset + data_size > mesh->surfaces[p_surface]->skin_buffer_size);
	const uint8_t *r = p_data.ptr();

	glBindBuffer(GL_ARRAY_BUFFER, mesh->surfaces[p_surface]->skin_buffer);
	glBufferSubData(GL_ARRAY_BUFFER, p_offset, data_size, r);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void MeshStorage::mesh_surface_set_material(RID p_mesh, int p_surface, RID p_material) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND(!mesh);
	ERR_FAIL_UNSIGNED_INDEX((uint32_t)p_surface, mesh->surface_count);
	mesh->surfaces[p_surface]->material = p_material;

	mesh->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MATERIAL);
	mesh->material_cache.clear();
}

RID MeshStorage::mesh_surface_get_material(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND_V(!mesh, RID());
	ERR_FAIL_UNSIGNED_INDEX_V((uint32_t)p_surface, mesh->surface_count, RID());

	return mesh->surfaces[p_surface]->material;
}

RS::SurfaceData MeshStorage::mesh_get_surface(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND_V(!mesh, RS::SurfaceData());
	ERR_FAIL_UNSIGNED_INDEX_V((uint32_t)p_surface, mesh->surface_count, RS::SurfaceData());

	Mesh::Surface &s = *mesh->surfaces[p_surface];

	RS::SurfaceData sd;
	sd.format = s.format;
	if (s.vertex_buffer != 0) {
		sd.vertex_data = Utilities::buffer_get_data(GL_ARRAY_BUFFER, s.vertex_buffer, s.vertex_buffer_size);
	}

	if (s.attribute_buffer != 0) {
		sd.attribute_data = Utilities::buffer_get_data(GL_ARRAY_BUFFER, s.attribute_buffer, s.attribute_buffer_size);
	}

	if (s.skin_buffer != 0) {
		sd.skin_data = Utilities::buffer_get_data(GL_ARRAY_BUFFER, s.skin_buffer, s.skin_buffer_size);
	}

	sd.vertex_count = s.vertex_count;
	sd.index_count = s.index_count;
	sd.primitive = s.primitive;

	if (sd.index_count) {
		sd.index_data = Utilities::buffer_get_data(GL_ELEMENT_ARRAY_BUFFER, s.index_buffer, s.index_buffer_size);
	}

	sd.aabb = s.aabb;
	for (uint32_t i = 0; i < s.lod_count; i++) {
		RS::SurfaceData::LOD lod;
		lod.edge_length = s.lods[i].edge_length;
		lod.index_data = Utilities::buffer_get_data(GL_ELEMENT_ARRAY_BUFFER, s.lods[i].index_buffer, s.lods[i].index_buffer_size);
		sd.lods.push_back(lod);
	}

	sd.bone_aabbs = s.bone_aabbs;

	if (mesh->blend_shape_count) {
		sd.blend_shape_data = Vector<uint8_t>();
		for (uint32_t i = 0; i < mesh->blend_shape_count; i++) {
			sd.blend_shape_data.append_array(Utilities::buffer_get_data(GL_ARRAY_BUFFER, s.blend_shapes[i].vertex_buffer, s.vertex_buffer_size));
		}
	}

	return sd;
}

int MeshStorage::mesh_get_surface_count(RID p_mesh) const {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND_V(!mesh, 0);
	return mesh->surface_count;
}

void MeshStorage::mesh_set_custom_aabb(RID p_mesh, const AABB &p_aabb) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND(!mesh);
	mesh->custom_aabb = p_aabb;
}

AABB MeshStorage::mesh_get_custom_aabb(RID p_mesh) const {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND_V(!mesh, AABB());
	return mesh->custom_aabb;
}

AABB MeshStorage::mesh_get_aabb(RID p_mesh, RID p_skeleton) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND_V(!mesh, AABB());

	if (mesh->custom_aabb != AABB()) {
		return mesh->custom_aabb;
	}

	Skeleton *skeleton = skeleton_owner.get_or_null(p_skeleton);

	if (!skeleton || skeleton->size == 0) {
		return mesh->aabb;
	}

	// Calculate AABB based on Skeleton

	AABB aabb;

	for (uint32_t i = 0; i < mesh->surface_count; i++) {
		AABB laabb;
		if ((mesh->surfaces[i]->format & RS::ARRAY_FORMAT_BONES) && mesh->surfaces[i]->bone_aabbs.size()) {
			int bs = mesh->surfaces[i]->bone_aabbs.size();
			const AABB *skbones = mesh->surfaces[i]->bone_aabbs.ptr();

			int sbs = skeleton->size;
			ERR_CONTINUE(bs > sbs);
			const float *baseptr = skeleton->data.ptr();

			bool first = true;

			if (skeleton->use_2d) {
				for (int j = 0; j < bs; j++) {
					if (skbones[0].size == Vector3()) {
						continue; //bone is unused
					}

					const float *dataptr = baseptr + j * 8;

					Transform3D mtx;

					mtx.basis.rows[0].x = dataptr[0];
					mtx.basis.rows[1].x = dataptr[1];
					mtx.origin.x = dataptr[3];

					mtx.basis.rows[0].y = dataptr[4];
					mtx.basis.rows[1].y = dataptr[5];
					mtx.origin.y = dataptr[7];

					AABB baabb = mtx.xform(skbones[j]);

					if (first) {
						laabb = baabb;
						first = false;
					} else {
						laabb.merge_with(baabb);
					}
				}
			} else {
				for (int j = 0; j < bs; j++) {
					if (skbones[0].size == Vector3()) {
						continue; //bone is unused
					}

					const float *dataptr = baseptr + j * 12;

					Transform3D mtx;

					mtx.basis.rows[0][0] = dataptr[0];
					mtx.basis.rows[0][1] = dataptr[1];
					mtx.basis.rows[0][2] = dataptr[2];
					mtx.origin.x = dataptr[3];
					mtx.basis.rows[1][0] = dataptr[4];
					mtx.basis.rows[1][1] = dataptr[5];
					mtx.basis.rows[1][2] = dataptr[6];
					mtx.origin.y = dataptr[7];
					mtx.basis.rows[2][0] = dataptr[8];
					mtx.basis.rows[2][1] = dataptr[9];
					mtx.basis.rows[2][2] = dataptr[10];
					mtx.origin.z = dataptr[11];

					AABB baabb = mtx.xform(skbones[j]);
					if (first) {
						laabb = baabb;
						first = false;
					} else {
						laabb.merge_with(baabb);
					}
				}
			}

			if (laabb.size == Vector3()) {
				laabb = mesh->surfaces[i]->aabb;
			}
		} else {
			laabb = mesh->surfaces[i]->aabb;
		}

		if (i == 0) {
			aabb = laabb;
		} else {
			aabb.merge_with(laabb);
		}
	}

	return aabb;
}

void MeshStorage::mesh_set_shadow_mesh(RID p_mesh, RID p_shadow_mesh) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND(!mesh);

	Mesh *shadow_mesh = mesh_owner.get_or_null(mesh->shadow_mesh);
	if (shadow_mesh) {
		shadow_mesh->shadow_owners.erase(mesh);
	}
	mesh->shadow_mesh = p_shadow_mesh;

	shadow_mesh = mesh_owner.get_or_null(mesh->shadow_mesh);

	if (shadow_mesh) {
		shadow_mesh->shadow_owners.insert(mesh);
	}

	mesh->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);
}

void MeshStorage::mesh_clear(RID p_mesh) {
	Mesh *mesh = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_COND(!mesh);
	for (uint32_t i = 0; i < mesh->surface_count; i++) {
		Mesh::Surface &s = *mesh->surfaces[i];

		if (s.vertex_buffer != 0) {
			glDeleteBuffers(1, &s.vertex_buffer);
			s.vertex_buffer = 0;
		}

		if (s.version_count != 0) {
			for (uint32_t j = 0; j < s.version_count; j++) {
				glDeleteVertexArrays(1, &s.versions[j].vertex_array);
				s.versions[j].vertex_array = 0;
			}
		}

		if (s.attribute_buffer != 0) {
			glDeleteBuffers(1, &s.attribute_buffer);
			s.attribute_buffer = 0;
		}

		if (s.skin_buffer != 0) {
			glDeleteBuffers(1, &s.skin_buffer);
			s.skin_buffer = 0;
		}

		if (s.index_buffer != 0) {
			glDeleteBuffers(1, &s.index_buffer);
			s.index_buffer = 0;
		}

		if (s.versions) {
			memfree(s.versions); //reallocs, so free with memfree.
		}

		if (s.lod_count) {
			for (uint32_t j = 0; j < s.lod_count; j++) {
				if (s.lods[j].index_buffer != 0) {
					glDeleteBuffers(1, &s.lods[j].index_buffer);
					s.lods[j].index_buffer = 0;
				}
			}
			memdelete_arr(s.lods);
		}

		if (mesh->blend_shape_count) {
			for (uint32_t j = 0; j < mesh->blend_shape_count; j++) {
				if (s.blend_shapes[j].vertex_buffer != 0) {
					glDeleteBuffers(1, &s.blend_shapes[j].vertex_buffer);
					s.blend_shapes[j].vertex_buffer = 0;
				}
				if (s.blend_shapes[j].vertex_array != 0) {
					glDeleteVertexArrays(1, &s.blend_shapes[j].vertex_array);
					s.blend_shapes[j].vertex_array = 0;
				}
			}
			memdelete_arr(s.blend_shapes);
		}
		if (s.skeleton_vertex_array != 0) {
			glDeleteVertexArrays(1, &s.skeleton_vertex_array);
			s.skeleton_vertex_array = 0;
		}

		memdelete(mesh->surfaces[i]);
	}
	if (mesh->surfaces) {
		memfree(mesh->surfaces);
	}

	mesh->surfaces = nullptr;
	mesh->surface_count = 0;
	mesh->material_cache.clear();
	//clear instance data
	for (MeshInstance *mi : mesh->instances) {
		_mesh_instance_clear(mi);
	}
	mesh->has_bone_weights = false;
	mesh->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);

	for (Mesh *E : mesh->shadow_owners) {
		Mesh *shadow_owner = E;
		shadow_owner->shadow_mesh = RID();
		shadow_owner->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);
	}
}

void MeshStorage::_mesh_surface_generate_version_for_input_mask(Mesh::Surface::Version &v, Mesh::Surface *s, uint32_t p_input_mask, MeshInstance::Surface *mis) {
	Mesh::Surface::Attrib attribs[RS::ARRAY_MAX];

	int attributes_stride = 0;
	int vertex_stride = 0;
	int skin_stride = 0;

	for (int i = 0; i < RS::ARRAY_INDEX; i++) {
		if (!(s->format & (1 << i))) {
			attribs[i].enabled = false;
			attribs[i].integer = false;
			continue;
		}

		attribs[i].enabled = true;
		attribs[i].integer = false;

		switch (i) {
			case RS::ARRAY_VERTEX: {
				attribs[i].offset = vertex_stride;
				if (s->format & RS::ARRAY_FLAG_USE_2D_VERTICES) {
					attribs[i].size = 2;
				} else {
					attribs[i].size = 3;
				}
				attribs[i].type = GL_FLOAT;
				vertex_stride += attribs[i].size * sizeof(float);
				attribs[i].normalized = GL_FALSE;
			} break;
			case RS::ARRAY_NORMAL: {
				attribs[i].offset = vertex_stride;
				attribs[i].size = 2;
				attribs[i].type = (mis ? GL_FLOAT : GL_UNSIGNED_SHORT);
				vertex_stride += sizeof(uint16_t) * 2 * (mis ? 2 : 1);
				attribs[i].normalized = GL_TRUE;
			} break;
			case RS::ARRAY_TANGENT: {
				attribs[i].offset = vertex_stride;
				attribs[i].size = 2;
				attribs[i].type = (mis ? GL_FLOAT : GL_UNSIGNED_SHORT);
				vertex_stride += sizeof(uint16_t) * 2 * (mis ? 2 : 1);
				attribs[i].normalized = GL_TRUE;
			} break;
			case RS::ARRAY_COLOR: {
				attribs[i].offset = attributes_stride;
				attribs[i].size = 4;
				attribs[i].type = GL_UNSIGNED_BYTE;
				attributes_stride += 4;
				attribs[i].normalized = GL_TRUE;
			} break;
			case RS::ARRAY_TEX_UV: {
				attribs[i].offset = attributes_stride;
				attribs[i].size = 2;
				attribs[i].type = GL_FLOAT;
				attributes_stride += 2 * sizeof(float);
				attribs[i].normalized = GL_FALSE;
			} break;
			case RS::ARRAY_TEX_UV2: {
				attribs[i].offset = attributes_stride;
				attribs[i].size = 2;
				attribs[i].type = GL_FLOAT;
				attributes_stride += 2 * sizeof(float);
				attribs[i].normalized = GL_FALSE;
			} break;
			case RS::ARRAY_CUSTOM0:
			case RS::ARRAY_CUSTOM1:
			case RS::ARRAY_CUSTOM2:
			case RS::ARRAY_CUSTOM3: {
				attribs[i].offset = attributes_stride;

				int idx = i - RS::ARRAY_CUSTOM0;
				uint32_t fmt_shift[RS::ARRAY_CUSTOM_COUNT] = { RS::ARRAY_FORMAT_CUSTOM0_SHIFT, RS::ARRAY_FORMAT_CUSTOM1_SHIFT, RS::ARRAY_FORMAT_CUSTOM2_SHIFT, RS::ARRAY_FORMAT_CUSTOM3_SHIFT };
				uint32_t fmt = (s->format >> fmt_shift[idx]) & RS::ARRAY_FORMAT_CUSTOM_MASK;
				uint32_t fmtsize[RS::ARRAY_CUSTOM_MAX] = { 4, 4, 4, 8, 4, 8, 12, 16 };
				GLenum gl_type[RS::ARRAY_CUSTOM_MAX] = { GL_UNSIGNED_BYTE, GL_BYTE, GL_HALF_FLOAT, GL_HALF_FLOAT, GL_FLOAT, GL_FLOAT, GL_FLOAT, GL_FLOAT };
				GLboolean norm[RS::ARRAY_CUSTOM_MAX] = { GL_TRUE, GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE };
				attribs[i].type = gl_type[fmt];
				attributes_stride += fmtsize[fmt];
				attribs[i].size = fmtsize[fmt] / sizeof(float);
				attribs[i].normalized = norm[fmt];
			} break;
			case RS::ARRAY_BONES: {
				attribs[i].offset = skin_stride;
				attribs[i].size = 4;
				attribs[i].type = GL_UNSIGNED_SHORT;
				skin_stride += 4 * sizeof(uint16_t);
				attribs[i].normalized = GL_FALSE;
				attribs[i].integer = true;
			} break;
			case RS::ARRAY_WEIGHTS: {
				attribs[i].offset = skin_stride;
				attribs[i].size = 4;
				attribs[i].type = GL_UNSIGNED_SHORT;
				skin_stride += 4 * sizeof(uint16_t);
				attribs[i].normalized = GL_TRUE;
			} break;
		}
	}

	glGenVertexArrays(1, &v.vertex_array);
	glBindVertexArray(v.vertex_array);

	for (int i = 0; i < RS::ARRAY_INDEX; i++) {
		if (!attribs[i].enabled) {
			glDisableVertexAttribArray(i);
			continue;
		}
		if (i <= RS::ARRAY_TANGENT) {
			attribs[i].stride = vertex_stride;
			if (mis) {
				glBindBuffer(GL_ARRAY_BUFFER, mis->vertex_buffer);
			} else {
				glBindBuffer(GL_ARRAY_BUFFER, s->vertex_buffer);
			}
		} else if (i <= RS::ARRAY_CUSTOM3) {
			attribs[i].stride = attributes_stride;
			glBindBuffer(GL_ARRAY_BUFFER, s->attribute_buffer);
		} else {
			attribs[i].stride = skin_stride;
			glBindBuffer(GL_ARRAY_BUFFER, s->skin_buffer);
		}

		if (attribs[i].integer) {
			glVertexAttribIPointer(i, attribs[i].size, attribs[i].type, attribs[i].stride, CAST_INT_TO_UCHAR_PTR(attribs[i].offset));
		} else {
			glVertexAttribPointer(i, attribs[i].size, attribs[i].type, attribs[i].normalized, attribs[i].stride, CAST_INT_TO_UCHAR_PTR(attribs[i].offset));
		}
		glEnableVertexAttribArray(i);
	}

	// Do not bind index here as we want to switch between index buffers for LOD

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	v.input_mask = p_input_mask;
}

/* MESH INSTANCE API */

RID MeshStorage::mesh_instance_create(RID p_base) {
	Mesh *mesh = mesh_owner.get_or_null(p_base);
	ERR_FAIL_COND_V(!mesh, RID());

	RID rid = mesh_instance_owner.make_rid();
	MeshInstance *mi = mesh_instance_owner.get_or_null(rid);

	mi->mesh = mesh;

	for (uint32_t i = 0; i < mesh->surface_count; i++) {
		_mesh_instance_add_surface(mi, mesh, i);
	}

	mi->I = mesh->instances.push_back(mi);

	mi->dirty = true;

	return rid;
}

void MeshStorage::mesh_instance_free(RID p_rid) {
	MeshInstance *mi = mesh_instance_owner.get_or_null(p_rid);
	_mesh_instance_clear(mi);
	mi->mesh->instances.erase(mi->I);
	mi->I = nullptr;

	mesh_instance_owner.free(p_rid);
}

void MeshStorage::mesh_instance_set_skeleton(RID p_mesh_instance, RID p_skeleton) {
	MeshInstance *mi = mesh_instance_owner.get_or_null(p_mesh_instance);
	if (mi->skeleton == p_skeleton) {
		return;
	}
	mi->skeleton = p_skeleton;
	mi->skeleton_version = 0;
	mi->dirty = true;
}

void MeshStorage::mesh_instance_set_blend_shape_weight(RID p_mesh_instance, int p_shape, float p_weight) {
	MeshInstance *mi = mesh_instance_owner.get_or_null(p_mesh_instance);
	ERR_FAIL_COND(!mi);
	ERR_FAIL_INDEX(p_shape, (int)mi->blend_weights.size());
	mi->blend_weights[p_shape] = p_weight;
	mi->dirty = true;
}

void MeshStorage::_mesh_instance_clear(MeshInstance *mi) {
	for (uint32_t i = 0; i < mi->surfaces.size(); i++) {
		if (mi->surfaces[i].version_count != 0) {
			for (uint32_t j = 0; j < mi->surfaces[i].version_count; j++) {
				glDeleteVertexArrays(1, &mi->surfaces[i].versions[j].vertex_array);
				mi->surfaces[i].versions[j].vertex_array = 0;
			}
			memfree(mi->surfaces[i].versions);
		}

		if (mi->surfaces[i].vertex_buffers[0] != 0) {
			glDeleteBuffers(2, mi->surfaces[i].vertex_buffers);
			mi->surfaces[i].vertex_buffers[0] = 0;
			mi->surfaces[i].vertex_buffers[1] = 0;
		}

		if (mi->surfaces[i].vertex_buffer != 0) {
			glDeleteBuffers(1, &mi->surfaces[i].vertex_buffer);
			mi->surfaces[i].vertex_buffer = 0;
		}
	}
	mi->surfaces.clear();
	mi->blend_weights.clear();
	mi->skeleton_version = 0;
}

void MeshStorage::_mesh_instance_add_surface(MeshInstance *mi, Mesh *mesh, uint32_t p_surface) {
	if (mesh->blend_shape_count > 0) {
		mi->blend_weights.resize(mesh->blend_shape_count);
		for (uint32_t i = 0; i < mi->blend_weights.size(); i++) {
			mi->blend_weights[i] = 0.0;
		}
	}

	MeshInstance::Surface s;
	if ((mesh->blend_shape_count > 0 || (mesh->surfaces[p_surface]->format & RS::ARRAY_FORMAT_BONES)) && mesh->surfaces[p_surface]->vertex_buffer_size > 0) {
		// Cache surface properties
		s.format_cache = mesh->surfaces[p_surface]->format;
		if ((s.format_cache & (1 << RS::ARRAY_VERTEX))) {
			if (s.format_cache & RS::ARRAY_FLAG_USE_2D_VERTICES) {
				s.vertex_size_cache = 2;
			} else {
				s.vertex_size_cache = 3;
			}
			s.vertex_stride_cache = sizeof(float) * s.vertex_size_cache;
		}
		if ((s.format_cache & (1 << RS::ARRAY_NORMAL))) {
			s.vertex_normal_offset_cache = s.vertex_stride_cache;
			s.vertex_stride_cache += sizeof(uint32_t) * 2;
		}
		if ((s.format_cache & (1 << RS::ARRAY_TANGENT))) {
			s.vertex_tangent_offset_cache = s.vertex_stride_cache;
			s.vertex_stride_cache += sizeof(uint32_t) * 2;
		}

		// Buffer to be used for rendering. Final output of skeleton and blend shapes.
		glGenBuffers(1, &s.vertex_buffer);
		glBindBuffer(GL_ARRAY_BUFFER, s.vertex_buffer);
		glBufferData(GL_ARRAY_BUFFER, s.vertex_stride_cache * mesh->surfaces[p_surface]->vertex_count, nullptr, GL_DYNAMIC_DRAW);
		if (mesh->blend_shape_count > 0) {
			// Ping-Pong buffers for processing blendshapes.
			glGenBuffers(2, s.vertex_buffers);
			for (uint32_t i = 0; i < 2; i++) {
				glBindBuffer(GL_ARRAY_BUFFER, s.vertex_buffers[i]);
				glBufferData(GL_ARRAY_BUFFER, s.vertex_stride_cache * mesh->surfaces[p_surface]->vertex_count, nullptr, GL_DYNAMIC_DRAW);
			}
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0); //unbind
	}

	mi->surfaces.push_back(s);
	mi->dirty = true;
}

void MeshStorage::mesh_instance_check_for_update(RID p_mesh_instance) {
	MeshInstance *mi = mesh_instance_owner.get_or_null(p_mesh_instance);

	bool needs_update = mi->dirty;

	if (mi->array_update_list.in_list()) {
		return;
	}

	if (!needs_update && mi->skeleton.is_valid()) {
		Skeleton *sk = skeleton_owner.get_or_null(mi->skeleton);
		if (sk && sk->version != mi->skeleton_version) {
			needs_update = true;
		}
	}

	if (needs_update) {
		dirty_mesh_instance_arrays.add(&mi->array_update_list);
	}
}

void MeshStorage::_blend_shape_bind_mesh_instance_buffer(MeshInstance *p_mi, uint32_t p_surface) {
	glBindBuffer(GL_ARRAY_BUFFER, p_mi->surfaces[p_surface].vertex_buffers[0]);

	if ((p_mi->surfaces[p_surface].format_cache & (1 << RS::ARRAY_VERTEX))) {
		glEnableVertexAttribArray(RS::ARRAY_VERTEX);
		glVertexAttribPointer(RS::ARRAY_VERTEX, p_mi->surfaces[p_surface].vertex_size_cache, GL_FLOAT, GL_FALSE, p_mi->surfaces[p_surface].vertex_stride_cache, CAST_INT_TO_UCHAR_PTR(0));
	} else {
		glDisableVertexAttribArray(RS::ARRAY_VERTEX);
	}
	if ((p_mi->surfaces[p_surface].format_cache & (1 << RS::ARRAY_NORMAL))) {
		glEnableVertexAttribArray(RS::ARRAY_NORMAL);
		glVertexAttribIPointer(RS::ARRAY_NORMAL, 2, GL_UNSIGNED_INT, p_mi->surfaces[p_surface].vertex_stride_cache, CAST_INT_TO_UCHAR_PTR(p_mi->surfaces[p_surface].vertex_normal_offset_cache));
	} else {
		glDisableVertexAttribArray(RS::ARRAY_NORMAL);
	}
	if ((p_mi->surfaces[p_surface].format_cache & (1 << RS::ARRAY_TANGENT))) {
		glEnableVertexAttribArray(RS::ARRAY_TANGENT);
		glVertexAttribIPointer(RS::ARRAY_TANGENT, 2, GL_UNSIGNED_INT, p_mi->surfaces[p_surface].vertex_stride_cache, CAST_INT_TO_UCHAR_PTR(p_mi->surfaces[p_surface].vertex_tangent_offset_cache));
	} else {
		glDisableVertexAttribArray(RS::ARRAY_TANGENT);
	}
}

void MeshStorage::_compute_skeleton(MeshInstance *p_mi, Skeleton *p_sk, uint32_t p_surface) {
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// Add in the bones and weights.
	glBindBuffer(GL_ARRAY_BUFFER, p_mi->mesh->surfaces[p_surface]->skin_buffer);

	bool use_8_weights = p_mi->surfaces[p_surface].format_cache & RS::ARRAY_FLAG_USE_8_BONE_WEIGHTS;
	int skin_stride = sizeof(int16_t) * (use_8_weights ? 16 : 8);
	glEnableVertexAttribArray(RS::ARRAY_BONES);
	glVertexAttribIPointer(RS::ARRAY_BONES, 4, GL_UNSIGNED_SHORT, skin_stride, CAST_INT_TO_UCHAR_PTR(0));
	if (use_8_weights) {
		glEnableVertexAttribArray(11);
		glVertexAttribIPointer(11, 4, GL_UNSIGNED_SHORT, skin_stride, CAST_INT_TO_UCHAR_PTR(4 * sizeof(uint16_t)));
		glEnableVertexAttribArray(12);
		glVertexAttribPointer(12, 4, GL_UNSIGNED_SHORT, GL_TRUE, skin_stride, CAST_INT_TO_UCHAR_PTR(8 * sizeof(uint16_t)));
		glEnableVertexAttribArray(13);
		glVertexAttribPointer(13, 4, GL_UNSIGNED_SHORT, GL_TRUE, skin_stride, CAST_INT_TO_UCHAR_PTR(12 * sizeof(uint16_t)));
	} else {
		glEnableVertexAttribArray(RS::ARRAY_WEIGHTS);
		glVertexAttribPointer(RS::ARRAY_WEIGHTS, 4, GL_UNSIGNED_SHORT, GL_TRUE, skin_stride, CAST_INT_TO_UCHAR_PTR(4 * sizeof(uint16_t)));
	}

	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, p_mi->surfaces[p_surface].vertex_buffer);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, p_sk->transforms_texture);

	glBeginTransformFeedback(GL_POINTS);
	glDrawArrays(GL_POINTS, 0, p_mi->mesh->surfaces[p_surface]->vertex_count);
	glEndTransformFeedback();

	glDisableVertexAttribArray(RS::ARRAY_BONES);
	glDisableVertexAttribArray(RS::ARRAY_WEIGHTS);
	glDisableVertexAttribArray(RS::ARRAY_BONES + 2);
	glDisableVertexAttribArray(RS::ARRAY_WEIGHTS + 2);
	glBindVertexArray(0);
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
}

void MeshStorage::update_mesh_instances() {
	if (dirty_mesh_instance_arrays.first() == nullptr) {
		return; //nothing to do
	}

	glEnable(GL_RASTERIZER_DISCARD);
	// Process skeletons and blend shapes using transform feedback
	while (dirty_mesh_instance_arrays.first()) {
		MeshInstance *mi = dirty_mesh_instance_arrays.first()->self();

		Skeleton *sk = skeleton_owner.get_or_null(mi->skeleton);

		// Precompute base weight if using blend shapes.
		float base_weight = 1.0;
		if (mi->mesh->blend_shape_count && mi->mesh->blend_shape_mode == RS::BLEND_SHAPE_MODE_NORMALIZED) {
			for (uint32_t i = 0; i < mi->mesh->blend_shape_count; i++) {
				base_weight -= mi->blend_weights[i];
			}
		}

		for (uint32_t i = 0; i < mi->surfaces.size(); i++) {
			if (mi->surfaces[i].vertex_buffer == 0 || mi->mesh->surfaces[i]->skeleton_vertex_array == 0) {
				continue;
			}

			bool array_is_2d = mi->surfaces[i].format_cache & RS::ARRAY_FLAG_USE_2D_VERTICES;
			bool can_use_skeleton = sk != nullptr && sk->use_2d == array_is_2d && (mi->surfaces[i].format_cache & RS::ARRAY_FORMAT_BONES);
			bool use_8_weights = mi->surfaces[i].format_cache & RS::ARRAY_FLAG_USE_8_BONE_WEIGHTS;

			// Always process blend shapes first.
			if (mi->mesh->blend_shape_count) {
				SkeletonShaderGLES3::ShaderVariant variant = SkeletonShaderGLES3::MODE_BASE_PASS;
				uint64_t specialization = 0;
				specialization |= array_is_2d ? SkeletonShaderGLES3::MODE_2D : 0;
				specialization |= SkeletonShaderGLES3::USE_BLEND_SHAPES;
				if (!array_is_2d) {
					if ((mi->surfaces[i].format_cache & (1 << RS::ARRAY_NORMAL))) {
						specialization |= SkeletonShaderGLES3::USE_NORMAL;
					}
					if ((mi->surfaces[i].format_cache & (1 << RS::ARRAY_TANGENT))) {
						specialization |= SkeletonShaderGLES3::USE_TANGENT;
					}
				}

				bool success = skeleton_shader.shader.version_bind_shader(skeleton_shader.shader_version, variant, specialization);
				if (!success) {
					continue;
				}

				skeleton_shader.shader.version_set_uniform(SkeletonShaderGLES3::BLEND_WEIGHT, base_weight, skeleton_shader.shader_version, variant, specialization);
				skeleton_shader.shader.version_set_uniform(SkeletonShaderGLES3::BLEND_SHAPE_COUNT, float(mi->mesh->blend_shape_count), skeleton_shader.shader_version, variant, specialization);

				glBindBuffer(GL_ARRAY_BUFFER, 0);
				glBindVertexArray(mi->mesh->surfaces[i]->skeleton_vertex_array);
				glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, mi->surfaces[i].vertex_buffers[0]);
				glBeginTransformFeedback(GL_POINTS);
				glDrawArrays(GL_POINTS, 0, mi->mesh->surfaces[i]->vertex_count);
				glEndTransformFeedback();

				variant = SkeletonShaderGLES3::MODE_BLEND_PASS;
				success = skeleton_shader.shader.version_bind_shader(skeleton_shader.shader_version, variant, specialization);
				if (!success) {
					continue;
				}

				//Do the last blend shape separately, as it can be combined with the skeleton pass.
				for (uint32_t bs = 0; bs < mi->mesh->blend_shape_count - 1; bs++) {
					float weight = mi->blend_weights[bs];

					if (Math::is_zero_approx(weight)) {
						//not bother with this one
						continue;
					}
					skeleton_shader.shader.version_set_uniform(SkeletonShaderGLES3::BLEND_WEIGHT, weight, skeleton_shader.shader_version, variant, specialization);
					skeleton_shader.shader.version_set_uniform(SkeletonShaderGLES3::BLEND_SHAPE_COUNT, float(mi->mesh->blend_shape_count), skeleton_shader.shader_version, variant, specialization);

					glBindVertexArray(mi->mesh->surfaces[i]->blend_shapes[bs].vertex_array);
					_blend_shape_bind_mesh_instance_buffer(mi, i);
					glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, mi->surfaces[i].vertex_buffers[1]);

					glBeginTransformFeedback(GL_POINTS);
					glDrawArrays(GL_POINTS, 0, mi->mesh->surfaces[i]->vertex_count);
					glEndTransformFeedback();

					SWAP(mi->surfaces[i].vertex_buffers[0], mi->surfaces[i].vertex_buffers[1]);
				}
				uint32_t bs = mi->mesh->blend_shape_count - 1;

				float weight = mi->blend_weights[bs];

				glBindVertexArray(mi->mesh->surfaces[i]->blend_shapes[bs].vertex_array);
				_blend_shape_bind_mesh_instance_buffer(mi, i);

				specialization |= can_use_skeleton ? SkeletonShaderGLES3::USE_SKELETON : 0;
				specialization |= (can_use_skeleton && use_8_weights) ? SkeletonShaderGLES3::USE_EIGHT_WEIGHTS : 0;
				specialization |= SkeletonShaderGLES3::FINAL_PASS;
				success = skeleton_shader.shader.version_bind_shader(skeleton_shader.shader_version, variant, specialization);
				if (!success) {
					continue;
				}

				skeleton_shader.shader.version_set_uniform(SkeletonShaderGLES3::BLEND_WEIGHT, weight, skeleton_shader.shader_version, variant, specialization);
				skeleton_shader.shader.version_set_uniform(SkeletonShaderGLES3::BLEND_SHAPE_COUNT, float(mi->mesh->blend_shape_count), skeleton_shader.shader_version, variant, specialization);

				if (can_use_skeleton) {
					// Do last blendshape in the same pass as the Skeleton.
					_compute_skeleton(mi, sk, i);
					can_use_skeleton = false;
				} else {
					// Do last blendshape by itself and prepare vertex data for use by the renderer.
					glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, mi->surfaces[i].vertex_buffer);

					glBeginTransformFeedback(GL_POINTS);
					glDrawArrays(GL_POINTS, 0, mi->mesh->surfaces[i]->vertex_count);
					glEndTransformFeedback();
				}

				glBindVertexArray(0);
				glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
			}

			// This branch should only execute when Skeleton is run by itself.
			if (can_use_skeleton) {
				SkeletonShaderGLES3::ShaderVariant variant = SkeletonShaderGLES3::MODE_BASE_PASS;
				uint64_t specialization = 0;
				specialization |= array_is_2d ? SkeletonShaderGLES3::MODE_2D : 0;
				specialization |= SkeletonShaderGLES3::USE_SKELETON;
				specialization |= SkeletonShaderGLES3::FINAL_PASS;
				specialization |= use_8_weights ? SkeletonShaderGLES3::USE_EIGHT_WEIGHTS : 0;
				if (!array_is_2d) {
					if ((mi->surfaces[i].format_cache & (1 << RS::ARRAY_NORMAL))) {
						specialization |= SkeletonShaderGLES3::USE_NORMAL;
					}
					if ((mi->surfaces[i].format_cache & (1 << RS::ARRAY_TANGENT))) {
						specialization |= SkeletonShaderGLES3::USE_TANGENT;
					}
				}

				bool success = skeleton_shader.shader.version_bind_shader(skeleton_shader.shader_version, variant, specialization);
				if (!success) {
					continue;
				}

				glBindVertexArray(mi->mesh->surfaces[i]->skeleton_vertex_array);
				_compute_skeleton(mi, sk, i);
			}
		}
		mi->dirty = false;
		if (sk) {
			mi->skeleton_version = sk->version;
		}
		dirty_mesh_instance_arrays.remove(&mi->array_update_list);
	}
	glDisable(GL_RASTERIZER_DISCARD);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
}

/* MULTIMESH API */

RID MeshStorage::multimesh_allocate() {
	return multimesh_owner.allocate_rid();
}

void MeshStorage::multimesh_initialize(RID p_rid) {
	multimesh_owner.initialize_rid(p_rid, MultiMesh());
}

void MeshStorage::multimesh_free(RID p_rid) {
	_update_dirty_multimeshes();
	multimesh_allocate_data(p_rid, 0, RS::MULTIMESH_TRANSFORM_2D);
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_rid);
	multimesh->dependency.deleted_notify(p_rid);
	multimesh_owner.free(p_rid);
}

void MeshStorage::multimesh_allocate_data(RID p_multimesh, int p_instances, RS::MultimeshTransformFormat p_transform_format, bool p_use_colors, bool p_use_custom_data) {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND(!multimesh);

	if (multimesh->instances == p_instances && multimesh->xform_format == p_transform_format && multimesh->uses_colors == p_use_colors && multimesh->uses_custom_data == p_use_custom_data) {
		return;
	}

	if (multimesh->buffer) {
		glDeleteBuffers(1, &multimesh->buffer);
		multimesh->buffer = 0;
	}

	if (multimesh->data_cache_dirty_regions) {
		memdelete_arr(multimesh->data_cache_dirty_regions);
		multimesh->data_cache_dirty_regions = nullptr;
		multimesh->data_cache_used_dirty_regions = 0;
	}

	multimesh->instances = p_instances;
	multimesh->xform_format = p_transform_format;
	multimesh->uses_colors = p_use_colors;
	multimesh->color_offset_cache = p_transform_format == RS::MULTIMESH_TRANSFORM_2D ? 8 : 12;
	multimesh->uses_custom_data = p_use_custom_data;
	multimesh->custom_data_offset_cache = multimesh->color_offset_cache + (p_use_colors ? 2 : 0);
	multimesh->stride_cache = multimesh->custom_data_offset_cache + (p_use_custom_data ? 2 : 0);
	multimesh->buffer_set = false;

	multimesh->data_cache = Vector<float>();
	multimesh->aabb = AABB();
	multimesh->aabb_dirty = false;
	multimesh->visible_instances = MIN(multimesh->visible_instances, multimesh->instances);

	if (multimesh->instances) {
		glGenBuffers(1, &multimesh->buffer);
		glBindBuffer(GL_ARRAY_BUFFER, multimesh->buffer);
		glBufferData(GL_ARRAY_BUFFER, multimesh->instances * multimesh->stride_cache * sizeof(float), nullptr, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	multimesh->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MULTIMESH);
}

int MeshStorage::multimesh_get_instance_count(RID p_multimesh) const {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND_V(!multimesh, 0);
	return multimesh->instances;
}

void MeshStorage::multimesh_set_mesh(RID p_multimesh, RID p_mesh) {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND(!multimesh);
	if (multimesh->mesh == p_mesh || p_mesh.is_null()) {
		return;
	}
	multimesh->mesh = p_mesh;

	if (multimesh->instances == 0) {
		return;
	}

	if (multimesh->data_cache.size()) {
		//we have a data cache, just mark it dirty
		_multimesh_mark_all_dirty(multimesh, false, true);
	} else if (multimesh->instances) {
		// Need to re-create AABB. Unfortunately, calling this has a penalty.
		if (multimesh->buffer_set) {
			Vector<uint8_t> buffer = Utilities::buffer_get_data(GL_ARRAY_BUFFER, multimesh->buffer, multimesh->instances * multimesh->stride_cache * sizeof(float));
			const uint8_t *r = buffer.ptr();
			const float *data = (const float *)r;
			_multimesh_re_create_aabb(multimesh, data, multimesh->instances);
		}
	}

	multimesh->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);
}

#define MULTIMESH_DIRTY_REGION_SIZE 512

void MeshStorage::_multimesh_make_local(MultiMesh *multimesh) const {
	if (multimesh->data_cache.size() > 0 || multimesh->instances == 0) {
		return; //already local
	}
	ERR_FAIL_COND(multimesh->data_cache.size() > 0);
	// this means that the user wants to load/save individual elements,
	// for this, the data must reside on CPU, so just copy it there.
	multimesh->data_cache.resize(multimesh->instances * multimesh->stride_cache);
	{
		float *w = multimesh->data_cache.ptrw();

		if (multimesh->buffer_set) {
			Vector<uint8_t> buffer = Utilities::buffer_get_data(GL_ARRAY_BUFFER, multimesh->buffer, multimesh->instances * multimesh->stride_cache * sizeof(float));

			{
				const uint8_t *r = buffer.ptr();
				memcpy(w, r, buffer.size());
			}
		} else {
			memset(w, 0, (size_t)multimesh->instances * multimesh->stride_cache * sizeof(float));
		}
	}
	uint32_t data_cache_dirty_region_count = (multimesh->instances - 1) / MULTIMESH_DIRTY_REGION_SIZE + 1;
	multimesh->data_cache_dirty_regions = memnew_arr(bool, data_cache_dirty_region_count);
	for (uint32_t i = 0; i < data_cache_dirty_region_count; i++) {
		multimesh->data_cache_dirty_regions[i] = false;
	}
	multimesh->data_cache_used_dirty_regions = 0;
}

void MeshStorage::_multimesh_mark_dirty(MultiMesh *multimesh, int p_index, bool p_aabb) {
	uint32_t region_index = p_index / MULTIMESH_DIRTY_REGION_SIZE;
#ifdef DEBUG_ENABLED
	uint32_t data_cache_dirty_region_count = (multimesh->instances - 1) / MULTIMESH_DIRTY_REGION_SIZE + 1;
	ERR_FAIL_UNSIGNED_INDEX(region_index, data_cache_dirty_region_count); //bug
#endif
	if (!multimesh->data_cache_dirty_regions[region_index]) {
		multimesh->data_cache_dirty_regions[region_index] = true;
		multimesh->data_cache_used_dirty_regions++;
	}

	if (p_aabb) {
		multimesh->aabb_dirty = true;
	}

	if (!multimesh->dirty) {
		multimesh->dirty_list = multimesh_dirty_list;
		multimesh_dirty_list = multimesh;
		multimesh->dirty = true;
	}
}

void MeshStorage::_multimesh_mark_all_dirty(MultiMesh *multimesh, bool p_data, bool p_aabb) {
	if (p_data) {
		uint32_t data_cache_dirty_region_count = (multimesh->instances - 1) / MULTIMESH_DIRTY_REGION_SIZE + 1;

		for (uint32_t i = 0; i < data_cache_dirty_region_count; i++) {
			if (!multimesh->data_cache_dirty_regions[i]) {
				multimesh->data_cache_dirty_regions[i] = true;
				multimesh->data_cache_used_dirty_regions++;
			}
		}
	}

	if (p_aabb) {
		multimesh->aabb_dirty = true;
	}

	if (!multimesh->dirty) {
		multimesh->dirty_list = multimesh_dirty_list;
		multimesh_dirty_list = multimesh;
		multimesh->dirty = true;
	}
}

void MeshStorage::_multimesh_re_create_aabb(MultiMesh *multimesh, const float *p_data, int p_instances) {
	ERR_FAIL_COND(multimesh->mesh.is_null());
	AABB aabb;
	AABB mesh_aabb = mesh_get_aabb(multimesh->mesh);
	for (int i = 0; i < p_instances; i++) {
		const float *data = p_data + multimesh->stride_cache * i;
		Transform3D t;

		if (multimesh->xform_format == RS::MULTIMESH_TRANSFORM_3D) {
			t.basis.rows[0][0] = data[0];
			t.basis.rows[0][1] = data[1];
			t.basis.rows[0][2] = data[2];
			t.origin.x = data[3];
			t.basis.rows[1][0] = data[4];
			t.basis.rows[1][1] = data[5];
			t.basis.rows[1][2] = data[6];
			t.origin.y = data[7];
			t.basis.rows[2][0] = data[8];
			t.basis.rows[2][1] = data[9];
			t.basis.rows[2][2] = data[10];
			t.origin.z = data[11];

		} else {
			t.basis.rows[0].x = data[0];
			t.basis.rows[1].x = data[1];
			t.origin.x = data[3];

			t.basis.rows[0].y = data[4];
			t.basis.rows[1].y = data[5];
			t.origin.y = data[7];
		}

		if (i == 0) {
			aabb = t.xform(mesh_aabb);
		} else {
			aabb.merge_with(t.xform(mesh_aabb));
		}
	}

	multimesh->aabb = aabb;
}

void MeshStorage::multimesh_instance_set_transform(RID p_multimesh, int p_index, const Transform3D &p_transform) {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND(!multimesh);
	ERR_FAIL_INDEX(p_index, multimesh->instances);
	ERR_FAIL_COND(multimesh->xform_format != RS::MULTIMESH_TRANSFORM_3D);

	_multimesh_make_local(multimesh);

	{
		float *w = multimesh->data_cache.ptrw();

		float *dataptr = w + p_index * multimesh->stride_cache;

		dataptr[0] = p_transform.basis.rows[0][0];
		dataptr[1] = p_transform.basis.rows[0][1];
		dataptr[2] = p_transform.basis.rows[0][2];
		dataptr[3] = p_transform.origin.x;
		dataptr[4] = p_transform.basis.rows[1][0];
		dataptr[5] = p_transform.basis.rows[1][1];
		dataptr[6] = p_transform.basis.rows[1][2];
		dataptr[7] = p_transform.origin.y;
		dataptr[8] = p_transform.basis.rows[2][0];
		dataptr[9] = p_transform.basis.rows[2][1];
		dataptr[10] = p_transform.basis.rows[2][2];
		dataptr[11] = p_transform.origin.z;
	}

	_multimesh_mark_dirty(multimesh, p_index, true);
}

void MeshStorage::multimesh_instance_set_transform_2d(RID p_multimesh, int p_index, const Transform2D &p_transform) {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND(!multimesh);
	ERR_FAIL_INDEX(p_index, multimesh->instances);
	ERR_FAIL_COND(multimesh->xform_format != RS::MULTIMESH_TRANSFORM_2D);

	_multimesh_make_local(multimesh);

	{
		float *w = multimesh->data_cache.ptrw();

		float *dataptr = w + p_index * multimesh->stride_cache;

		dataptr[0] = p_transform.columns[0][0];
		dataptr[1] = p_transform.columns[1][0];
		dataptr[2] = 0;
		dataptr[3] = p_transform.columns[2][0];
		dataptr[4] = p_transform.columns[0][1];
		dataptr[5] = p_transform.columns[1][1];
		dataptr[6] = 0;
		dataptr[7] = p_transform.columns[2][1];
	}

	_multimesh_mark_dirty(multimesh, p_index, true);
}

void MeshStorage::multimesh_instance_set_color(RID p_multimesh, int p_index, const Color &p_color) {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND(!multimesh);
	ERR_FAIL_INDEX(p_index, multimesh->instances);
	ERR_FAIL_COND(!multimesh->uses_colors);

	_multimesh_make_local(multimesh);

	{
		// Colors are packed into 2 floats.
		float *w = multimesh->data_cache.ptrw();

		float *dataptr = w + p_index * multimesh->stride_cache + multimesh->color_offset_cache;
		uint16_t val[4] = { Math::make_half_float(p_color.r), Math::make_half_float(p_color.g), Math::make_half_float(p_color.b), Math::make_half_float(p_color.a) };
		memcpy(dataptr, val, 2 * 4);
	}

	_multimesh_mark_dirty(multimesh, p_index, false);
}

void MeshStorage::multimesh_instance_set_custom_data(RID p_multimesh, int p_index, const Color &p_color) {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND(!multimesh);
	ERR_FAIL_INDEX(p_index, multimesh->instances);
	ERR_FAIL_COND(!multimesh->uses_custom_data);

	_multimesh_make_local(multimesh);

	{
		float *w = multimesh->data_cache.ptrw();

		float *dataptr = w + p_index * multimesh->stride_cache + multimesh->custom_data_offset_cache;
		uint16_t val[4] = { Math::make_half_float(p_color.r), Math::make_half_float(p_color.g), Math::make_half_float(p_color.b), Math::make_half_float(p_color.a) };
		memcpy(dataptr, val, 2 * 4);
	}

	_multimesh_mark_dirty(multimesh, p_index, false);
}

RID MeshStorage::multimesh_get_mesh(RID p_multimesh) const {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND_V(!multimesh, RID());

	return multimesh->mesh;
}

AABB MeshStorage::multimesh_get_aabb(RID p_multimesh) const {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND_V(!multimesh, AABB());
	if (multimesh->aabb_dirty) {
		const_cast<MeshStorage *>(this)->_update_dirty_multimeshes();
	}
	return multimesh->aabb;
}

Transform3D MeshStorage::multimesh_instance_get_transform(RID p_multimesh, int p_index) const {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND_V(!multimesh, Transform3D());
	ERR_FAIL_INDEX_V(p_index, multimesh->instances, Transform3D());
	ERR_FAIL_COND_V(multimesh->xform_format != RS::MULTIMESH_TRANSFORM_3D, Transform3D());

	_multimesh_make_local(multimesh);

	Transform3D t;
	{
		const float *r = multimesh->data_cache.ptr();

		const float *dataptr = r + p_index * multimesh->stride_cache;

		t.basis.rows[0][0] = dataptr[0];
		t.basis.rows[0][1] = dataptr[1];
		t.basis.rows[0][2] = dataptr[2];
		t.origin.x = dataptr[3];
		t.basis.rows[1][0] = dataptr[4];
		t.basis.rows[1][1] = dataptr[5];
		t.basis.rows[1][2] = dataptr[6];
		t.origin.y = dataptr[7];
		t.basis.rows[2][0] = dataptr[8];
		t.basis.rows[2][1] = dataptr[9];
		t.basis.rows[2][2] = dataptr[10];
		t.origin.z = dataptr[11];
	}

	return t;
}

Transform2D MeshStorage::multimesh_instance_get_transform_2d(RID p_multimesh, int p_index) const {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND_V(!multimesh, Transform2D());
	ERR_FAIL_INDEX_V(p_index, multimesh->instances, Transform2D());
	ERR_FAIL_COND_V(multimesh->xform_format != RS::MULTIMESH_TRANSFORM_2D, Transform2D());

	_multimesh_make_local(multimesh);

	Transform2D t;
	{
		const float *r = multimesh->data_cache.ptr();

		const float *dataptr = r + p_index * multimesh->stride_cache;

		t.columns[0][0] = dataptr[0];
		t.columns[1][0] = dataptr[1];
		t.columns[2][0] = dataptr[3];
		t.columns[0][1] = dataptr[4];
		t.columns[1][1] = dataptr[5];
		t.columns[2][1] = dataptr[7];
	}

	return t;
}

Color MeshStorage::multimesh_instance_get_color(RID p_multimesh, int p_index) const {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND_V(!multimesh, Color());
	ERR_FAIL_INDEX_V(p_index, multimesh->instances, Color());
	ERR_FAIL_COND_V(!multimesh->uses_colors, Color());

	_multimesh_make_local(multimesh);

	Color c;
	{
		const float *r = multimesh->data_cache.ptr();

		const float *dataptr = r + p_index * multimesh->stride_cache + multimesh->color_offset_cache;
		uint16_t raw_data[4];
		memcpy(raw_data, dataptr, 2 * 4);
		c.r = Math::half_to_float(raw_data[0]);
		c.g = Math::half_to_float(raw_data[1]);
		c.b = Math::half_to_float(raw_data[2]);
		c.a = Math::half_to_float(raw_data[3]);
	}

	return c;
}

Color MeshStorage::multimesh_instance_get_custom_data(RID p_multimesh, int p_index) const {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND_V(!multimesh, Color());
	ERR_FAIL_INDEX_V(p_index, multimesh->instances, Color());
	ERR_FAIL_COND_V(!multimesh->uses_custom_data, Color());

	_multimesh_make_local(multimesh);

	Color c;
	{
		const float *r = multimesh->data_cache.ptr();

		const float *dataptr = r + p_index * multimesh->stride_cache + multimesh->custom_data_offset_cache;
		uint16_t raw_data[4];
		memcpy(raw_data, dataptr, 2 * 4);
		c.r = Math::half_to_float(raw_data[0]);
		c.g = Math::half_to_float(raw_data[1]);
		c.b = Math::half_to_float(raw_data[2]);
		c.a = Math::half_to_float(raw_data[3]);
	}

	return c;
}

void MeshStorage::multimesh_set_buffer(RID p_multimesh, const Vector<float> &p_buffer) {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND(!multimesh);

	if (multimesh->uses_colors || multimesh->uses_custom_data) {
		// Color and custom need to be packed so copy buffer to data_cache and pack.

		_multimesh_make_local(multimesh);
		multimesh->data_cache = p_buffer;

		float *w = multimesh->data_cache.ptrw();
		uint32_t old_stride = multimesh->xform_format == RS::MULTIMESH_TRANSFORM_2D ? 8 : 12;
		old_stride += multimesh->uses_colors ? 4 : 0;
		old_stride += multimesh->uses_custom_data ? 4 : 0;
		for (int i = 0; i < multimesh->instances; i++) {
			{
				float *dataptr = w + i * old_stride;
				float *newptr = w + i * multimesh->stride_cache;
				float vals[8] = { dataptr[0], dataptr[1], dataptr[2], dataptr[3], dataptr[4], dataptr[5], dataptr[6], dataptr[7] };
				memcpy(newptr, vals, 8 * 4);
			}

			if (multimesh->xform_format == RS::MULTIMESH_TRANSFORM_3D) {
				float *dataptr = w + i * old_stride + 8;
				float *newptr = w + i * multimesh->stride_cache + 8;
				float vals[8] = { dataptr[0], dataptr[1], dataptr[2], dataptr[3] };
				memcpy(newptr, vals, 4 * 4);
			}

			if (multimesh->uses_colors) {
				float *dataptr = w + i * old_stride + (multimesh->xform_format == RS::MULTIMESH_TRANSFORM_2D ? 8 : 12);
				float *newptr = w + i * multimesh->stride_cache + multimesh->color_offset_cache;
				uint16_t val[4] = { Math::make_half_float(dataptr[0]), Math::make_half_float(dataptr[1]), Math::make_half_float(dataptr[2]), Math::make_half_float(dataptr[3]) };
				memcpy(newptr, val, 2 * 4);
			}
			if (multimesh->uses_custom_data) {
				float *dataptr = w + i * old_stride + (multimesh->xform_format == RS::MULTIMESH_TRANSFORM_2D ? 8 : 12) + (multimesh->uses_colors ? 4 : 0);
				float *newptr = w + i * multimesh->stride_cache + multimesh->custom_data_offset_cache;
				uint16_t val[4] = { Math::make_half_float(dataptr[0]), Math::make_half_float(dataptr[1]), Math::make_half_float(dataptr[2]), Math::make_half_float(dataptr[3]) };
				memcpy(newptr, val, 2 * 4);
			}
		}

		multimesh->data_cache.resize(multimesh->instances * (int)multimesh->stride_cache);
		const float *r = multimesh->data_cache.ptr();
		glBindBuffer(GL_ARRAY_BUFFER, multimesh->buffer);
		glBufferData(GL_ARRAY_BUFFER, multimesh->data_cache.size() * sizeof(float), r, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

	} else {
		// Only Transform is being used, so we can upload directly.
		ERR_FAIL_COND(p_buffer.size() != (multimesh->instances * (int)multimesh->stride_cache));
		const float *r = p_buffer.ptr();
		glBindBuffer(GL_ARRAY_BUFFER, multimesh->buffer);
		glBufferData(GL_ARRAY_BUFFER, p_buffer.size() * sizeof(float), r, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	multimesh->buffer_set = true;

	if (multimesh->data_cache.size() || multimesh->uses_colors || multimesh->uses_custom_data) {
		//if we have a data cache, just update it
		multimesh->data_cache = multimesh->data_cache;
		{
			//clear dirty since nothing will be dirty anymore
			uint32_t data_cache_dirty_region_count = (multimesh->instances - 1) / MULTIMESH_DIRTY_REGION_SIZE + 1;
			for (uint32_t i = 0; i < data_cache_dirty_region_count; i++) {
				multimesh->data_cache_dirty_regions[i] = false;
			}
			multimesh->data_cache_used_dirty_regions = 0;
		}

		_multimesh_mark_all_dirty(multimesh, false, true); //update AABB
	} else if (multimesh->mesh.is_valid()) {
		//if we have a mesh set, we need to re-generate the AABB from the new data
		const float *data = p_buffer.ptr();

		_multimesh_re_create_aabb(multimesh, data, multimesh->instances);
		multimesh->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_AABB);
	}
}

Vector<float> MeshStorage::multimesh_get_buffer(RID p_multimesh) const {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND_V(!multimesh, Vector<float>());
	Vector<float> ret;
	if (multimesh->buffer == 0 || multimesh->instances == 0) {
		return Vector<float>();
	} else if (multimesh->data_cache.size()) {
		ret = multimesh->data_cache;
	} else {
		// Buffer not cached, so fetch from GPU memory. This can be a stalling operation, avoid whenever possible.

		Vector<uint8_t> buffer = Utilities::buffer_get_data(GL_ARRAY_BUFFER, multimesh->buffer, multimesh->instances * multimesh->stride_cache * sizeof(float));
		ret.resize(multimesh->instances * multimesh->stride_cache);
		{
			float *w = ret.ptrw();
			const uint8_t *r = buffer.ptr();
			memcpy(w, r, buffer.size());
		}
	}
	if (multimesh->uses_colors || multimesh->uses_custom_data) {
		// Need to decompress buffer.
		uint32_t new_stride = multimesh->xform_format == RS::MULTIMESH_TRANSFORM_2D ? 8 : 12;
		new_stride += multimesh->uses_colors ? 4 : 0;
		new_stride += multimesh->uses_custom_data ? 4 : 0;

		Vector<float> decompressed;
		decompressed.resize(multimesh->instances * (int)new_stride);
		float *w = decompressed.ptrw();
		const float *r = ret.ptr();

		for (int i = 0; i < multimesh->instances; i++) {
			{
				float *newptr = w + i * new_stride;
				const float *oldptr = r + i * multimesh->stride_cache;
				float vals[8] = { oldptr[0], oldptr[1], oldptr[2], oldptr[3], oldptr[4], oldptr[5], oldptr[6], oldptr[7] };
				memcpy(newptr, vals, 8 * 4);
			}

			if (multimesh->xform_format == RS::MULTIMESH_TRANSFORM_3D) {
				float *newptr = w + i * new_stride + 8;
				const float *oldptr = r + i * multimesh->stride_cache + 8;
				float vals[8] = { oldptr[0], oldptr[1], oldptr[2], oldptr[3] };
				memcpy(newptr, vals, 4 * 4);
			}

			if (multimesh->uses_colors) {
				float *newptr = w + i * new_stride + (multimesh->xform_format == RS::MULTIMESH_TRANSFORM_2D ? 8 : 12);
				const float *oldptr = r + i * multimesh->stride_cache + multimesh->color_offset_cache;
				uint16_t raw_data[4];
				memcpy(raw_data, oldptr, 2 * 4);
				newptr[0] = Math::half_to_float(raw_data[0]);
				newptr[1] = Math::half_to_float(raw_data[1]);
				newptr[2] = Math::half_to_float(raw_data[2]);
				newptr[3] = Math::half_to_float(raw_data[3]);
			}
			if (multimesh->uses_custom_data) {
				float *newptr = w + i * new_stride + (multimesh->xform_format == RS::MULTIMESH_TRANSFORM_2D ? 8 : 12) + (multimesh->uses_colors ? 4 : 0);
				const float *oldptr = r + i * multimesh->stride_cache + multimesh->custom_data_offset_cache;
				uint16_t raw_data[4];
				memcpy(raw_data, oldptr, 2 * 4);
				newptr[0] = Math::half_to_float(raw_data[0]);
				newptr[1] = Math::half_to_float(raw_data[1]);
				newptr[2] = Math::half_to_float(raw_data[2]);
				newptr[3] = Math::half_to_float(raw_data[3]);
			}
		}
		return decompressed;
	} else {
		return ret;
	}
}

void MeshStorage::multimesh_set_visible_instances(RID p_multimesh, int p_visible) {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND(!multimesh);
	ERR_FAIL_COND(p_visible < -1 || p_visible > multimesh->instances);
	if (multimesh->visible_instances == p_visible) {
		return;
	}

	if (multimesh->data_cache.size()) {
		//there is a data cache..
		_multimesh_mark_all_dirty(multimesh, false, true);
	}

	multimesh->visible_instances = p_visible;

	multimesh->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MULTIMESH_VISIBLE_INSTANCES);
}

int MeshStorage::multimesh_get_visible_instances(RID p_multimesh) const {
	MultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_COND_V(!multimesh, 0);
	return multimesh->visible_instances;
}

void MeshStorage::_update_dirty_multimeshes() {
	while (multimesh_dirty_list) {
		MultiMesh *multimesh = multimesh_dirty_list;

		if (multimesh->data_cache.size()) { //may have been cleared, so only process if it exists
			const float *data = multimesh->data_cache.ptr();

			uint32_t visible_instances = multimesh->visible_instances >= 0 ? multimesh->visible_instances : multimesh->instances;

			if (multimesh->data_cache_used_dirty_regions) {
				uint32_t data_cache_dirty_region_count = (multimesh->instances - 1) / MULTIMESH_DIRTY_REGION_SIZE + 1;
				uint32_t visible_region_count = visible_instances == 0 ? 0 : (visible_instances - 1) / MULTIMESH_DIRTY_REGION_SIZE + 1;

				GLint region_size = multimesh->stride_cache * MULTIMESH_DIRTY_REGION_SIZE * sizeof(float);

				if (multimesh->data_cache_used_dirty_regions > 32 || multimesh->data_cache_used_dirty_regions > visible_region_count / 2) {
					// If there too many dirty regions, or represent the majority of regions, just copy all, else transfer cost piles up too much
					glBindBuffer(GL_ARRAY_BUFFER, multimesh->buffer);
					glBufferData(GL_ARRAY_BUFFER, MIN(visible_region_count * region_size, multimesh->instances * multimesh->stride_cache * sizeof(float)), data, GL_STATIC_DRAW);
					glBindBuffer(GL_ARRAY_BUFFER, 0);
				} else {
					// Not that many regions? update them all
					// TODO: profile the performance cost on low end
					glBindBuffer(GL_ARRAY_BUFFER, multimesh->buffer);
					for (uint32_t i = 0; i < visible_region_count; i++) {
						if (multimesh->data_cache_dirty_regions[i]) {
							GLint offset = i * region_size;
							GLint size = multimesh->stride_cache * (uint32_t)multimesh->instances * (uint32_t)sizeof(float);
							uint32_t region_start_index = multimesh->stride_cache * MULTIMESH_DIRTY_REGION_SIZE * i;
							glBufferSubData(GL_ARRAY_BUFFER, offset, MIN(region_size, size - offset), &data[region_start_index]);
						}
					}
					glBindBuffer(GL_ARRAY_BUFFER, 0);
				}

				for (uint32_t i = 0; i < data_cache_dirty_region_count; i++) {
					multimesh->data_cache_dirty_regions[i] = false;
				}

				multimesh->data_cache_used_dirty_regions = 0;
			}

			if (multimesh->aabb_dirty && multimesh->mesh.is_valid()) {
				_multimesh_re_create_aabb(multimesh, data, visible_instances);
				multimesh->aabb_dirty = false;
				multimesh->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_AABB);
			}
		}

		multimesh_dirty_list = multimesh->dirty_list;

		multimesh->dirty_list = nullptr;
		multimesh->dirty = false;
	}

	multimesh_dirty_list = nullptr;
}

/* SKELETON API */

RID MeshStorage::skeleton_allocate() {
	return skeleton_owner.allocate_rid();
}

void MeshStorage::skeleton_initialize(RID p_rid) {
	skeleton_owner.initialize_rid(p_rid, Skeleton());
}

void MeshStorage::skeleton_free(RID p_rid) {
	_update_dirty_skeletons();
	skeleton_allocate_data(p_rid, 0);
	Skeleton *skeleton = skeleton_owner.get_or_null(p_rid);
	skeleton->dependency.deleted_notify(p_rid);
	skeleton_owner.free(p_rid);
}

void MeshStorage::_skeleton_make_dirty(Skeleton *skeleton) {
	if (!skeleton->dirty) {
		skeleton->dirty = true;
		skeleton->dirty_list = skeleton_dirty_list;
		skeleton_dirty_list = skeleton;
	}
}

void MeshStorage::skeleton_allocate_data(RID p_skeleton, int p_bones, bool p_2d_skeleton) {
	Skeleton *skeleton = skeleton_owner.get_or_null(p_skeleton);
	ERR_FAIL_COND(!skeleton);
	ERR_FAIL_COND(p_bones < 0);

	if (skeleton->size == p_bones && skeleton->use_2d == p_2d_skeleton) {
		return;
	}

	skeleton->size = p_bones;
	skeleton->use_2d = p_2d_skeleton;
	skeleton->height = (p_bones * (p_2d_skeleton ? 2 : 3)) / 256;
	if ((p_bones * (p_2d_skeleton ? 2 : 3)) % 256) {
		skeleton->height++;
	}

	if (skeleton->transforms_texture != 0) {
		glDeleteTextures(1, &skeleton->transforms_texture);
		skeleton->transforms_texture = 0;
		skeleton->data.clear();
	}

	if (skeleton->size) {
		skeleton->data.resize(256 * skeleton->height * 4);
		glGenTextures(1, &skeleton->transforms_texture);
		glBindTexture(GL_TEXTURE_2D, skeleton->transforms_texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 256, skeleton->height, 0, GL_RGBA, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);

		memset(skeleton->data.ptrw(), 0, skeleton->data.size() * sizeof(float));

		_skeleton_make_dirty(skeleton);
	}

	skeleton->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_SKELETON_DATA);
}

void MeshStorage::skeleton_set_base_transform_2d(RID p_skeleton, const Transform2D &p_base_transform) {
	Skeleton *skeleton = skeleton_owner.get_or_null(p_skeleton);

	ERR_FAIL_NULL(skeleton);
	ERR_FAIL_COND(!skeleton->use_2d);

	skeleton->base_transform_2d = p_base_transform;
}

int MeshStorage::skeleton_get_bone_count(RID p_skeleton) const {
	Skeleton *skeleton = skeleton_owner.get_or_null(p_skeleton);
	ERR_FAIL_COND_V(!skeleton, 0);

	return skeleton->size;
}

void MeshStorage::skeleton_bone_set_transform(RID p_skeleton, int p_bone, const Transform3D &p_transform) {
	Skeleton *skeleton = skeleton_owner.get_or_null(p_skeleton);

	ERR_FAIL_COND(!skeleton);
	ERR_FAIL_INDEX(p_bone, skeleton->size);
	ERR_FAIL_COND(skeleton->use_2d);

	float *dataptr = skeleton->data.ptrw() + p_bone * 12;

	dataptr[0] = p_transform.basis.rows[0][0];
	dataptr[1] = p_transform.basis.rows[0][1];
	dataptr[2] = p_transform.basis.rows[0][2];
	dataptr[3] = p_transform.origin.x;
	dataptr[4] = p_transform.basis.rows[1][0];
	dataptr[5] = p_transform.basis.rows[1][1];
	dataptr[6] = p_transform.basis.rows[1][2];
	dataptr[7] = p_transform.origin.y;
	dataptr[8] = p_transform.basis.rows[2][0];
	dataptr[9] = p_transform.basis.rows[2][1];
	dataptr[10] = p_transform.basis.rows[2][2];
	dataptr[11] = p_transform.origin.z;

	_skeleton_make_dirty(skeleton);
}

Transform3D MeshStorage::skeleton_bone_get_transform(RID p_skeleton, int p_bone) const {
	Skeleton *skeleton = skeleton_owner.get_or_null(p_skeleton);

	ERR_FAIL_COND_V(!skeleton, Transform3D());
	ERR_FAIL_INDEX_V(p_bone, skeleton->size, Transform3D());
	ERR_FAIL_COND_V(skeleton->use_2d, Transform3D());

	const float *dataptr = skeleton->data.ptr() + p_bone * 12;

	Transform3D t;

	t.basis.rows[0][0] = dataptr[0];
	t.basis.rows[0][1] = dataptr[1];
	t.basis.rows[0][2] = dataptr[2];
	t.origin.x = dataptr[3];
	t.basis.rows[1][0] = dataptr[4];
	t.basis.rows[1][1] = dataptr[5];
	t.basis.rows[1][2] = dataptr[6];
	t.origin.y = dataptr[7];
	t.basis.rows[2][0] = dataptr[8];
	t.basis.rows[2][1] = dataptr[9];
	t.basis.rows[2][2] = dataptr[10];
	t.origin.z = dataptr[11];

	return t;
}

void MeshStorage::skeleton_bone_set_transform_2d(RID p_skeleton, int p_bone, const Transform2D &p_transform) {
	Skeleton *skeleton = skeleton_owner.get_or_null(p_skeleton);

	ERR_FAIL_COND(!skeleton);
	ERR_FAIL_INDEX(p_bone, skeleton->size);
	ERR_FAIL_COND(!skeleton->use_2d);

	float *dataptr = skeleton->data.ptrw() + p_bone * 8;

	dataptr[0] = p_transform.columns[0][0];
	dataptr[1] = p_transform.columns[1][0];
	dataptr[2] = 0;
	dataptr[3] = p_transform.columns[2][0];
	dataptr[4] = p_transform.columns[0][1];
	dataptr[5] = p_transform.columns[1][1];
	dataptr[6] = 0;
	dataptr[7] = p_transform.columns[2][1];

	_skeleton_make_dirty(skeleton);
}

Transform2D MeshStorage::skeleton_bone_get_transform_2d(RID p_skeleton, int p_bone) const {
	Skeleton *skeleton = skeleton_owner.get_or_null(p_skeleton);

	ERR_FAIL_COND_V(!skeleton, Transform2D());
	ERR_FAIL_INDEX_V(p_bone, skeleton->size, Transform2D());
	ERR_FAIL_COND_V(!skeleton->use_2d, Transform2D());

	const float *dataptr = skeleton->data.ptr() + p_bone * 8;

	Transform2D t;
	t.columns[0][0] = dataptr[0];
	t.columns[1][0] = dataptr[1];
	t.columns[2][0] = dataptr[3];
	t.columns[0][1] = dataptr[4];
	t.columns[1][1] = dataptr[5];
	t.columns[2][1] = dataptr[7];

	return t;
}

void MeshStorage::_update_dirty_skeletons() {
	while (skeleton_dirty_list) {
		Skeleton *skeleton = skeleton_dirty_list;

		if (skeleton->size) {
			glBindTexture(GL_TEXTURE_2D, skeleton->transforms_texture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 256, skeleton->height, 0, GL_RGBA, GL_FLOAT, skeleton->data.ptr());
			glBindTexture(GL_TEXTURE_2D, 0);
		}

		skeleton_dirty_list = skeleton->dirty_list;

		skeleton->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_SKELETON_BONES);

		skeleton->version++;

		skeleton->dirty = false;
		skeleton->dirty_list = nullptr;
	}

	skeleton_dirty_list = nullptr;
}

void MeshStorage::skeleton_update_dependency(RID p_skeleton, DependencyTracker *p_instance) {
	Skeleton *skeleton = skeleton_owner.get_or_null(p_skeleton);
	ERR_FAIL_COND(!skeleton);

	p_instance->update_dependency(&skeleton->dependency);
}

#endif // GLES3_ENABLED
