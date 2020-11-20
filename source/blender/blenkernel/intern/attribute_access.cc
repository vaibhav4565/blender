/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <utility>

#include "BKE_attribute_access.hh"
#include "BKE_customdata.h"
#include "BKE_deform.h"
#include "BKE_geometry_set.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_pointcloud_types.h"

#include "BLI_color.hh"
#include "BLI_float2.hh"
#include "BLI_span.hh"

namespace blender::bke {

/* -------------------------------------------------------------------- */
/** \name Attribute accessor implementations.
 * \{ */

ReadAttribute::~ReadAttribute() = default;
WriteAttribute::~WriteAttribute() = default;

class VertexWeightWriteAttribute final : public WriteAttribute {
 private:
  MutableSpan<MDeformVert> dverts_;
  const int dvert_index_;

 public:
  VertexWeightWriteAttribute(MDeformVert *dverts, const int totvert, const int dvert_index)
      : WriteAttribute(ATTR_DOMAIN_VERTEX, CPPType::get<float>(), totvert),
        dverts_(dverts, totvert),
        dvert_index_(dvert_index)
  {
  }

  void get_internal(const int64_t index, void *r_value) const override
  {
    get_internal(dverts_, dvert_index_, index, r_value);
  }

  void set_internal(const int64_t index, const void *value) override
  {
    MDeformWeight *weight = BKE_defvert_ensure_index(&dverts_[index], dvert_index_);
    weight->weight = *reinterpret_cast<const float *>(value);
  }

  static void get_internal(const Span<MDeformVert> dverts,
                           const int dvert_index,
                           const int64_t index,
                           void *r_value)
  {
    const MDeformVert &dvert = dverts[index];
    for (const MDeformWeight &weight : Span(dvert.dw, dvert.totweight)) {
      if (weight.def_nr == dvert_index) {
        *(float *)r_value = weight.weight;
        return;
      }
    }
    *(float *)r_value = 0.0f;
  }
};

class VertexWeightReadAttribute final : public ReadAttribute {
 private:
  const Span<MDeformVert> dverts_;
  const int dvert_index_;

 public:
  VertexWeightReadAttribute(const MDeformVert *dverts, const int totvert, const int dvert_index)
      : ReadAttribute(ATTR_DOMAIN_VERTEX, CPPType::get<float>(), totvert),
        dverts_(dverts, totvert),
        dvert_index_(dvert_index)
  {
  }

  void get_internal(const int64_t index, void *r_value) const override
  {
    VertexWeightWriteAttribute::get_internal(dverts_, dvert_index_, index, r_value);
  }
};

template<typename T> class ArrayWriteAttribute final : public WriteAttribute {
 private:
  MutableSpan<T> data_;

 public:
  ArrayWriteAttribute(AttributeDomain domain, MutableSpan<T> data)
      : WriteAttribute(domain, CPPType::get<T>(), data.size()), data_(data)
  {
  }

  void get_internal(const int64_t index, void *r_value) const override
  {
    new (r_value) T(data_[index]);
  }

  void set_internal(const int64_t index, const void *value) override
  {
    data_[index] = *reinterpret_cast<const T *>(value);
  }
};

template<typename T> class ArrayReadAttribute final : public ReadAttribute {
 private:
  Span<T> data_;

 public:
  ArrayReadAttribute(AttributeDomain domain, Span<T> data)
      : ReadAttribute(domain, CPPType::get<T>(), data.size()), data_(data)
  {
  }

  void get_internal(const int64_t index, void *r_value) const override
  {
    new (r_value) T(data_[index]);
  }
};

template<typename StructT, typename ElemT, typename GetFuncT, typename SetFuncT>
class DerivedArrayWriteAttribute final : public WriteAttribute {
 private:
  MutableSpan<StructT> data_;
  GetFuncT get_function_;
  SetFuncT set_function_;

 public:
  DerivedArrayWriteAttribute(AttributeDomain domain,
                             MutableSpan<StructT> data,
                             GetFuncT get_function,
                             SetFuncT set_function)
      : WriteAttribute(domain, CPPType::get<ElemT>(), data.size()),
        data_(data),
        get_function_(std::move(get_function)),
        set_function_(std::move(set_function))
  {
  }

  void get_internal(const int64_t index, void *r_value) const override
  {
    const StructT &struct_value = data_[index];
    const ElemT value = get_function_(struct_value);
    new (r_value) ElemT(value);
  }

  void set_internal(const int64_t index, const void *value) override
  {
    StructT &struct_value = data_[index];
    const ElemT &typed_value = *reinterpret_cast<const ElemT *>(value);
    set_function_(struct_value, typed_value);
  }
};

template<typename StructT, typename ElemT, typename GetFuncT>
class DerivedArrayReadAttribute final : public ReadAttribute {
 private:
  Span<StructT> data_;
  GetFuncT get_function_;

 public:
  DerivedArrayReadAttribute(AttributeDomain domain, Span<StructT> data, GetFuncT get_function)
      : ReadAttribute(domain, CPPType::get<ElemT>(), data.size()),
        data_(data),
        get_function_(std::move(get_function))
  {
  }

  void get_internal(const int64_t index, void *r_value) const override
  {
    const StructT &struct_value = data_[index];
    const ElemT value = get_function_(struct_value);
    new (r_value) ElemT(value);
  }
};

class ConstantReadAttribute final : public ReadAttribute {
 private:
  void *value_;

 public:
  ConstantReadAttribute(AttributeDomain domain,
                        const int64_t size,
                        const CPPType &type,
                        const void *value)
      : ReadAttribute(domain, type, size)
  {
    value_ = MEM_mallocN_aligned(type.size(), type.alignment(), __func__);
    type.copy_to_uninitialized(value, value_);
  }

  ~ConstantReadAttribute()
  {
    this->cpp_type_.destruct(value_);
    MEM_freeN(value_);
  }

  void get_internal(const int64_t UNUSED(index), void *r_value) const override
  {
    this->cpp_type_.copy_to_uninitialized(value_, r_value);
  }
};

/** \} */

const blender::fn::CPPType *custom_data_type_to_cpp_type(const CustomDataType type)
{
  switch (type) {
    case CD_PROP_FLOAT:
      return &CPPType::get<float>();
    case CD_PROP_FLOAT2:
      return &CPPType::get<float2>();
    case CD_PROP_FLOAT3:
      return &CPPType::get<float3>();
    case CD_PROP_INT32:
      return &CPPType::get<int>();
    case CD_PROP_COLOR:
      return &CPPType::get<Color4f>();
    default:
      return nullptr;
  }
  return nullptr;
}

CustomDataType cpp_type_to_custom_data_type(const blender::fn::CPPType &type)
{
  if (type.is<float>()) {
    return CD_PROP_FLOAT;
  }
  if (type.is<float2>()) {
    return CD_PROP_FLOAT2;
  }
  if (type.is<float3>()) {
    return CD_PROP_FLOAT3;
  }
  if (type.is<int>()) {
    return CD_PROP_INT32;
  }
  if (type.is<Color4f>()) {
    return CD_PROP_COLOR;
  }
  return static_cast<CustomDataType>(-1);
}

}  // namespace blender::bke

/* -------------------------------------------------------------------- */
/** \name Utilities for accessing attributes.
 * \{ */

using blender::float3;
using blender::StringRef;
using blender::bke::ReadAttributePtr;
using blender::bke::WriteAttributePtr;

static ReadAttributePtr read_attribute_from_custom_data(const CustomData &custom_data,
                                                        const int size,
                                                        const StringRef attribute_name,
                                                        const AttributeDomain domain)
{
  using namespace blender;
  using namespace blender::bke;
  for (const CustomDataLayer &layer : Span(custom_data.layers, custom_data.totlayer)) {
    if (layer.name != nullptr && layer.name == attribute_name) {
      switch (layer.type) {
        case CD_PROP_FLOAT:
          return std::make_unique<ArrayReadAttribute<float>>(
              domain, Span(static_cast<float *>(layer.data), size));
        case CD_PROP_FLOAT2:
          return std::make_unique<ArrayReadAttribute<float2>>(
              domain, Span(static_cast<float2 *>(layer.data), size));
        case CD_PROP_FLOAT3:
          return std::make_unique<ArrayReadAttribute<float3>>(
              domain, Span(static_cast<float3 *>(layer.data), size));
        case CD_PROP_INT32:
          return std::make_unique<ArrayReadAttribute<int>>(
              domain, Span(static_cast<int *>(layer.data), size));
        case CD_PROP_COLOR:
          return std::make_unique<ArrayReadAttribute<Color4f>>(
              domain, Span(static_cast<Color4f *>(layer.data), size));
      }
    }
  }
  return {};
}

static WriteAttributePtr write_attribute_from_custom_data(CustomData custom_data,
                                                          const int size,
                                                          const StringRef attribute_name,
                                                          const AttributeDomain domain)
{
  using namespace blender;
  using namespace blender::bke;
  for (const CustomDataLayer &layer : Span(custom_data.layers, custom_data.totlayer)) {
    if (layer.name != nullptr && layer.name == attribute_name) {
      switch (layer.type) {
        case CD_PROP_FLOAT:
          return std::make_unique<ArrayWriteAttribute<float>>(
              domain, MutableSpan(static_cast<float *>(layer.data), size));
        case CD_PROP_FLOAT2:
          return std::make_unique<ArrayWriteAttribute<float2>>(
              domain, MutableSpan(static_cast<float2 *>(layer.data), size));
        case CD_PROP_FLOAT3:
          return std::make_unique<ArrayWriteAttribute<float3>>(
              domain, MutableSpan(static_cast<float3 *>(layer.data), size));
        case CD_PROP_INT32:
          return std::make_unique<ArrayWriteAttribute<int>>(
              domain, MutableSpan(static_cast<int *>(layer.data), size));
        case CD_PROP_COLOR:
          return std::make_unique<ArrayWriteAttribute<Color4f>>(
              domain, MutableSpan(static_cast<Color4f *>(layer.data), size));
      }
    }
  }
  return {};
}

/* Returns true when the layer was found and is deleted. */
static bool delete_named_custom_data_layer(CustomData &custom_data,
                                           const StringRef attribute_name,
                                           const int size)
{
  for (const int index : blender::IndexRange(custom_data.totlayer)) {
    const CustomDataLayer &layer = custom_data.layers[index];
    if (layer.name == attribute_name) {
      CustomData_free_layer(&custom_data, layer.type, size, index);
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GeometryComponent
 * \{ */

bool GeometryComponent::attribute_domain_supported(const AttributeDomain UNUSED(domain)) const
{
  return false;
}

bool GeometryComponent::attribute_domain_with_type_supported(
    const AttributeDomain UNUSED(domain), const CustomDataType UNUSED(data_type)) const
{
  return false;
}

int GeometryComponent::attribute_domain_size(const AttributeDomain UNUSED(domain)) const
{
  BLI_assert(false);
  return 0;
}

bool GeometryComponent::attribute_is_builtin(const StringRef UNUSED(attribute_name)) const
{
  return true;
}

ReadAttributePtr GeometryComponent::attribute_try_get_for_read(
    const StringRef UNUSED(attribute_name)) const
{
  return {};
}

ReadAttributePtr GeometryComponent::attribute_try_adapt_domain(ReadAttributePtr attribute,
                                                               const AttributeDomain domain) const
{
  if (attribute && attribute->domain() == domain) {
    return attribute;
  }
  return {};
}

WriteAttributePtr GeometryComponent::attribute_try_get_for_write(
    const StringRef UNUSED(attribute_name))
{
  return {};
}

bool GeometryComponent::attribute_try_delete(const StringRef UNUSED(attribute_name))
{
  return false;
}

bool GeometryComponent::attribute_try_create(const StringRef UNUSED(attribute_name),
                                             const AttributeDomain UNUSED(domain),
                                             const CustomDataType UNUSED(data_type))
{
  return false;
}

ReadAttributePtr GeometryComponent::attribute_try_get_for_read(
    const StringRef attribute_name,
    const AttributeDomain domain,
    const CustomDataType data_type) const
{
  if (!this->attribute_domain_with_type_supported(domain, data_type)) {
    return {};
  }

  ReadAttributePtr attribute = this->attribute_try_get_for_read(attribute_name);
  if (!attribute) {
    return {};
  }

  if (attribute->domain() != domain) {
    attribute = this->attribute_try_adapt_domain(std::move(attribute), domain);
    if (!attribute) {
      return {};
    }
  }

  const blender::fn::CPPType *cpp_type = blender::bke::custom_data_type_to_cpp_type(data_type);
  BLI_assert(cpp_type != nullptr);
  if (attribute->cpp_type() != *cpp_type) {
    /* TODO: Support some type conversions. */
    return {};
  }

  return attribute;
}

ReadAttributePtr GeometryComponent::attribute_get_for_read(const StringRef attribute_name,
                                                           const AttributeDomain domain,
                                                           const CustomDataType data_type,
                                                           const void *default_value) const
{
  BLI_assert(this->attribute_domain_with_type_supported(domain, data_type));

  ReadAttributePtr attribute = this->attribute_try_get_for_read(attribute_name, domain, data_type);
  if (attribute) {
    return attribute;
  }

  const blender::fn::CPPType *cpp_type = blender::bke::custom_data_type_to_cpp_type(data_type);
  BLI_assert(cpp_type != nullptr);
  const int domain_size = this->attribute_domain_size(domain);
  return std::make_unique<blender::bke::ConstantReadAttribute>(
      domain, domain_size, *cpp_type, default_value);
}

WriteAttributePtr GeometryComponent::attribute_try_ensure_for_write(const StringRef attribute_name,
                                                                    const AttributeDomain domain,
                                                                    const CustomDataType data_type)
{
  const blender::fn::CPPType *cpp_type = blender::bke::custom_data_type_to_cpp_type(data_type);
  BLI_assert(cpp_type != nullptr);

  WriteAttributePtr attribute = this->attribute_try_get_for_write(attribute_name);
  if (attribute && attribute->domain() == domain && attribute->cpp_type() == *cpp_type) {
    return attribute;
  }

  if (attribute) {
    if (!this->attribute_try_delete(attribute_name)) {
      return {};
    }
  }
  if (!this->attribute_domain_with_type_supported(domain, data_type)) {
    return {};
  }
  if (!this->attribute_try_create(attribute_name, domain, data_type)) {
    return {};
  }
  return this->attribute_try_get_for_write(attribute_name);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PointCloudComponent
 * \{ */

bool PointCloudComponent::attribute_domain_supported(const AttributeDomain domain) const
{
  return domain == ATTR_DOMAIN_POINT;
}

bool PointCloudComponent::attribute_domain_with_type_supported(
    const AttributeDomain domain, const CustomDataType data_type) const
{
  return domain == ATTR_DOMAIN_POINT && ELEM(data_type,
                                             CD_PROP_FLOAT,
                                             CD_PROP_FLOAT2,
                                             CD_PROP_FLOAT3,
                                             CD_PROP_INT32,
                                             CD_PROP_COLOR);
}

int PointCloudComponent::attribute_domain_size(const AttributeDomain domain) const
{
  BLI_assert(domain == ATTR_DOMAIN_POINT);
  UNUSED_VARS_NDEBUG(domain);
  if (pointcloud_ == nullptr) {
    return 0;
  }
  return pointcloud_->totpoint;
}

bool PointCloudComponent::attribute_is_builtin(const StringRef attribute_name) const
{
  return attribute_name == "Position";
}

ReadAttributePtr PointCloudComponent::attribute_try_get_for_read(
    const StringRef attribute_name) const
{
  if (pointcloud_ == nullptr) {
    return {};
  }

  return read_attribute_from_custom_data(
      pointcloud_->pdata, pointcloud_->totpoint, attribute_name, ATTR_DOMAIN_POINT);
}

WriteAttributePtr PointCloudComponent::attribute_try_get_for_write(const StringRef attribute_name)
{
  PointCloud *pointcloud = this->get_for_write();
  if (pointcloud == nullptr) {
    return {};
  }

  return write_attribute_from_custom_data(
      pointcloud->pdata, pointcloud->totpoint, attribute_name, ATTR_DOMAIN_POINT);
}

bool PointCloudComponent::attribute_try_delete(const StringRef attribute_name)
{
  if (this->attribute_is_builtin(attribute_name)) {
    return false;
  }
  PointCloud *pointcloud = this->get_for_write();
  if (pointcloud == nullptr) {
    return false;
  }
  delete_named_custom_data_layer(pointcloud->pdata, attribute_name, pointcloud->totpoint);
  return true;
}

static bool custom_data_has_layer_with_name(const CustomData &custom_data, const StringRef name)
{
  for (const CustomDataLayer &layer : blender::Span(custom_data.layers, custom_data.totlayer)) {
    if (layer.name == name) {
      return true;
    }
  }
  return false;
}

bool PointCloudComponent::attribute_try_create(const StringRef attribute_name,
                                               const AttributeDomain domain,
                                               const CustomDataType data_type)
{
  if (this->attribute_is_builtin(attribute_name)) {
    return false;
  }
  if (!this->attribute_domain_with_type_supported(domain, data_type)) {
    return false;
  }
  PointCloud *pointcloud = this->get_for_write();
  if (pointcloud == nullptr) {
    return false;
  }
  if (custom_data_has_layer_with_name(pointcloud->pdata, attribute_name)) {
    return false;
  }

  char attribute_name_c[MAX_NAME];
  attribute_name.copy(attribute_name_c);
  CustomData_add_layer_named(
      &pointcloud->pdata, data_type, CD_DEFAULT, nullptr, pointcloud_->totpoint, attribute_name_c);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name MeshComponent
 * \{ */

bool MeshComponent::attribute_domain_supported(const AttributeDomain domain) const
{
  return ELEM(
      domain, ATTR_DOMAIN_CORNER, ATTR_DOMAIN_VERTEX, ATTR_DOMAIN_EDGE, ATTR_DOMAIN_POLYGON);
}

bool MeshComponent::attribute_domain_with_type_supported(const AttributeDomain domain,
                                                         const CustomDataType data_type) const
{
  if (!this->attribute_domain_supported(domain)) {
    return false;
  }
  return ELEM(
      data_type, CD_PROP_FLOAT, CD_PROP_FLOAT2, CD_PROP_FLOAT3, CD_PROP_INT32, CD_PROP_COLOR);
}

int MeshComponent::attribute_domain_size(const AttributeDomain domain) const
{
  BLI_assert(this->attribute_domain_supported(domain));
  if (mesh_ == nullptr) {
    return 0;
  }
  switch (domain) {
    case ATTR_DOMAIN_CORNER:
      return mesh_->totloop;
    case ATTR_DOMAIN_VERTEX:
      return mesh_->totvert;
    case ATTR_DOMAIN_EDGE:
      return mesh_->totedge;
    case ATTR_DOMAIN_POLYGON:
      return mesh_->totpoly;
    default:
      BLI_assert(false);
      break;
  }
  return 0;
}

bool MeshComponent::attribute_is_builtin(const StringRef attribute_name) const
{
  return attribute_name == "Position";
}

ReadAttributePtr MeshComponent::attribute_try_get_for_read(const StringRef attribute_name) const
{
  if (mesh_ == nullptr) {
    return {};
  }

  if (attribute_name == "Position") {
    auto get_vertex_position = [](const MVert &vert) { return float3(vert.co); };
    return std::make_unique<
        blender::bke::DerivedArrayReadAttribute<MVert, float3, decltype(get_vertex_position)>>(
        ATTR_DOMAIN_VERTEX, blender::Span(mesh_->mvert, mesh_->totvert), get_vertex_position);
  }

  ReadAttributePtr corner_attribute = read_attribute_from_custom_data(
      mesh_->ldata, mesh_->totloop, attribute_name, ATTR_DOMAIN_CORNER);
  if (corner_attribute) {
    return corner_attribute;
  }

  const int vertex_group_index = vertex_group_names_.lookup_default(attribute_name, -1);
  if (vertex_group_index >= 0) {
    return std::make_unique<blender::bke::VertexWeightReadAttribute>(
        mesh_->dvert, mesh_->totvert, vertex_group_index);
  }

  ReadAttributePtr vertex_attribute = read_attribute_from_custom_data(
      mesh_->vdata, mesh_->totvert, attribute_name, ATTR_DOMAIN_VERTEX);
  if (vertex_attribute) {
    return vertex_attribute;
  }

  ReadAttributePtr edge_attribute = read_attribute_from_custom_data(
      mesh_->edata, mesh_->totedge, attribute_name, ATTR_DOMAIN_EDGE);
  if (edge_attribute) {
    return edge_attribute;
  }

  ReadAttributePtr polygon_attribute = read_attribute_from_custom_data(
      mesh_->pdata, mesh_->totpoly, attribute_name, ATTR_DOMAIN_POLYGON);
  if (polygon_attribute) {
    return polygon_attribute;
  }

  return {};
}

WriteAttributePtr MeshComponent::attribute_try_get_for_write(const StringRef attribute_name)
{
  Mesh *mesh = this->get_for_write();
  if (mesh == nullptr) {
    return {};
  }

  if (attribute_name == "Position") {
    auto get_vertex_position = [](const MVert &vert) { return float3(vert.co); };
    auto set_vertex_position = [](MVert &vert, const float3 &co) { copy_v3_v3(vert.co, co); };
    return std::make_unique<
        blender::bke::DerivedArrayWriteAttribute<MVert,
                                                 float3,
                                                 decltype(get_vertex_position),
                                                 decltype(set_vertex_position)>>(
        ATTR_DOMAIN_VERTEX,
        blender::MutableSpan(mesh_->mvert, mesh_->totvert),
        get_vertex_position,
        set_vertex_position);
  }

  WriteAttributePtr corner_attribute = write_attribute_from_custom_data(
      mesh_->ldata, mesh_->totloop, attribute_name, ATTR_DOMAIN_CORNER);
  if (corner_attribute) {
    return corner_attribute;
  }

  const int vertex_group_index = vertex_group_names_.lookup_default_as(attribute_name, -1);
  if (vertex_group_index >= 0) {
    return std::make_unique<blender::bke::VertexWeightWriteAttribute>(
        mesh_->dvert, mesh_->totvert, vertex_group_index);
  }

  WriteAttributePtr vertex_attribute = write_attribute_from_custom_data(
      mesh_->vdata, mesh_->totvert, attribute_name, ATTR_DOMAIN_VERTEX);
  if (vertex_attribute) {
    return vertex_attribute;
  }

  WriteAttributePtr edge_attribute = write_attribute_from_custom_data(
      mesh_->edata, mesh_->totedge, attribute_name, ATTR_DOMAIN_EDGE);
  if (edge_attribute) {
    return edge_attribute;
  }

  WriteAttributePtr polygon_attribute = write_attribute_from_custom_data(
      mesh_->pdata, mesh_->totpoly, attribute_name, ATTR_DOMAIN_POLYGON);
  if (polygon_attribute) {
    return polygon_attribute;
  }

  return {};
}

bool MeshComponent::attribute_try_delete(const StringRef attribute_name)
{
  if (this->attribute_is_builtin(attribute_name)) {
    return false;
  }
  Mesh *mesh = this->get_for_write();
  if (mesh == nullptr) {
    return false;
  }

  delete_named_custom_data_layer(mesh_->ldata, attribute_name, mesh_->totloop);
  delete_named_custom_data_layer(mesh_->vdata, attribute_name, mesh_->totvert);
  delete_named_custom_data_layer(mesh_->edata, attribute_name, mesh_->totedge);
  delete_named_custom_data_layer(mesh_->pdata, attribute_name, mesh_->totpoly);

  const int vertex_group_index = vertex_group_names_.lookup_default_as(attribute_name, -1);
  if (vertex_group_index != -1) {
    for (MDeformVert &dvert : blender::MutableSpan(mesh_->dvert, mesh_->totvert)) {
      MDeformWeight *weight = BKE_defvert_find_index(&dvert, vertex_group_index);
      BKE_defvert_remove_group(&dvert, weight);
    }
    vertex_group_names_.remove_as(attribute_name);
  }

  return true;
}

bool MeshComponent::attribute_try_create(const StringRef attribute_name,
                                         const AttributeDomain domain,
                                         const CustomDataType data_type)
{
  if (this->attribute_is_builtin(attribute_name)) {
    return false;
  }
  if (!this->attribute_domain_with_type_supported(domain, data_type)) {
    return false;
  }
  Mesh *mesh = this->get_for_write();
  if (mesh == nullptr) {
    return false;
  }

  char attribute_name_c[MAX_NAME];
  attribute_name.copy(attribute_name_c);

  switch (domain) {
    case ATTR_DOMAIN_CORNER: {
      if (custom_data_has_layer_with_name(mesh->ldata, attribute_name)) {
        return false;
      }
      CustomData_add_layer_named(
          &mesh->ldata, data_type, CD_DEFAULT, nullptr, mesh->totloop, attribute_name_c);
      return true;
    }
    case ATTR_DOMAIN_VERTEX: {
      if (custom_data_has_layer_with_name(mesh->vdata, attribute_name)) {
        return false;
      }
      if (vertex_group_names_.contains_as(attribute_name)) {
        return false;
      }
      CustomData_add_layer_named(
          &mesh->vdata, data_type, CD_DEFAULT, nullptr, mesh->totvert, attribute_name_c);
      return true;
    }
    case ATTR_DOMAIN_EDGE: {
      if (custom_data_has_layer_with_name(mesh->edata, attribute_name)) {
        return false;
      }
      CustomData_add_layer_named(
          &mesh->edata, data_type, CD_DEFAULT, nullptr, mesh->totedge, attribute_name_c);
      return true;
    }
    case ATTR_DOMAIN_POLYGON: {
      if (custom_data_has_layer_with_name(mesh->pdata, attribute_name)) {
        return false;
      }
      CustomData_add_layer_named(
          &mesh->pdata, data_type, CD_DEFAULT, nullptr, mesh->totpoly, attribute_name_c);
      return true;
    }
    default:
      return false;
  }
}

/** \} */
