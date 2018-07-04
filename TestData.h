#pragma once

// ReSharper disable once CppUnusedIncludeDirective
#include <boost/serialization/unique_ptr.hpp>
#include <boost/serialization/array.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/export.hpp>

#include <boost/type_index.hpp>
#include <boost/type_index/runtime_cast.hpp>

#include <memory>
#include <array>
#include <vector>

//---------------------------------------------------------------------------------------------------------------------
// Static Data
//---------------------------------------------------------------------------------------------------------------------
struct ObjectData
{
    BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS(BOOST_TYPE_INDEX_NO_BASE_CLASS)

    virtual ~ObjectData() { }

    std::string name;
};

struct ModelObjectData : ObjectData
{
    BOOST_TYPE_INDEX_REGISTER_RUNTIME_CLASS((ObjectData))

    std::array<unsigned char, 120> modelPayload;
};

struct Data final
{
    // we should use shared because Undo\Redo queue can share ownership
    // when object has been removed from this vector
    // and we need polymorphism on ObjectData
    // so we cant use unique_ptr here

    // but for clarity sake here I use what I want
    std::vector<std::unique_ptr<ObjectData>> objects;

    float duration = 0.0f;
};

namespace boost {namespace serialization
{
template <typename Archive>
void serialize(Archive& ar, ObjectData& data, const uint32_t version)
{
    ar & data.name;
}

template <typename Archive>
void serialize(Archive& ar, ModelObjectData& data, const uint32_t version)
{
    // serialize base class information
    ar & serialization::base_object<ObjectData>(data);
    ar & data.modelPayload;
}

template <typename Archive>
void serialize(Archive& ar, Data& data, const uint32_t version)
{
    ar & data.objects;
    ar & data.duration;
}
}}
