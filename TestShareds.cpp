#include "stdafx.h"

#include "TestData.h"
#include "TestData.inl"
#include "ResManagement.h"

#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/format.hpp>

#include <string>
#include <memory>
#include <vector>
#include <array>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <iostream>
#include <sstream>

//---------------------------------------------------------------------------------------------------------------------
// Dynamic Data
//---------------------------------------------------------------------------------------------------------------------
// Generic object instance and support data
struct ObjectInstanceIntermediateData
{
    virtual ~ObjectInstanceIntermediateData() {}
};

class ObjectInstance : public IReloadable<ObjectData, std::unique_ptr<ObjectInstanceIntermediateData>>
{
public:
    explicit ObjectInstance(ObjectData& data)
        : m_data(data) { }

    virtual ~ObjectInstance() { }

    const std::string& name() const
    {
        return m_data.name;
    }

protected:
    ObjectData& m_data;
};

// Example model object instance and its support data
class ModelWrapper
{
public:
    virtual ~ModelWrapper()
    {
        std::cout << "Resource released: destroyed ModelWrapper\n Payload:\n";
        auto line = 0;
        auto fmt = boost::format("0x%02x");
        for (auto& i : payload)
        {
            std::cout << " " << fmt % int(i) << " ";
            if (++line == 8)
            {
                line = 0;
                std::cout << "\n";
            }
        }
        std::cout << "\n";
    }

    std::array<unsigned char, 16> payload;
};

struct ModelObjectInstanceIntermediateData : ObjectInstanceIntermediateData
{
    std::shared_ptr<ModelWrapper> wrapper;
};

class ModelObjectInstance : public ObjectInstance
{
public:
    explicit ModelObjectInstance(ModelObjectData& data)
        : ObjectInstance(data) { }

    ~ModelObjectInstance()
    {
        std::cout << "Resource released: destroyed ModelObjectInstance \"" << name() << "\"\n";
    }

    // Pass by value to document that I do want to share ownership
    void bind(std::shared_ptr<ModelWrapper> wrapper)
    {
        m_wrapped = std::move(wrapper);
    }

    std::unique_ptr<ObjectInstanceIntermediateData> prepareReload() override
    {
        auto state = std::make_unique<ModelObjectInstanceIntermediateData>();
        state->wrapper = wrapper();
        return std::move(state);
    }

    void reloadFromData(const std::unique_ptr<ObjectInstanceIntermediateData>& state, const ObjectData& data) override
    {
        m_data = data;
        auto& mystate = static_cast<ModelObjectInstanceIntermediateData&>(*state);
        m_wrapped = mystate.wrapper;
    }

    void requestReload() override { }

    void reloadDone() override { }

    std::shared_ptr<ModelWrapper> wrapper() const
    {
        return m_wrapped;
    }

private:
    std::shared_ptr<ModelWrapper> m_wrapped;
};

//---------------------------------------------------------------------------------------------------------------------
// Factory fro creation objects and monitoring their activity
//---------------------------------------------------------------------------------------------------------------------
class ObjectFactory
{
public:
    template <typename T, typename... Args>
    static std::unique_ptr<T> create(Args&&... args)
    {
        if constexpr (std::is_same_v<std::decay_t<T>, ModelObjectInstance>)
        {
            return createModel(args...);
        }
        return std::make_unique<T>(std::forward<Args>(args)...);
    }

    static std::unique_ptr<ModelObjectInstance> createModel(ModelObjectData& data)
    {
        return std::make_unique<ModelObjectInstance>(data);
    }
};

//---------------------------------------------------------------------------------------------------------------------
// Sequence
//---------------------------------------------------------------------------------------------------------------------
struct SequenceIntermediateData
{
    SequenceIntermediateData() { }

    ~SequenceIntermediateData() { }

    SequenceIntermediateData(const SequenceIntermediateData& other) = delete;

    SequenceIntermediateData& operator=(const SequenceIntermediateData& other) = delete;

    SequenceIntermediateData(SequenceIntermediateData&& other) noexcept
        : objects(std::move(other.objects)) {}

    SequenceIntermediateData& operator=(SequenceIntermediateData&& other) noexcept
    {
        if (this == &other)
            return *this;
        objects = std::move(other.objects);
        return *this;
    }

    std::unordered_map<std::string, std::unique_ptr<ObjectInstanceIntermediateData>> objects;
};

class Sequence final : public IReloadable<std::shared_ptr<Data>, SequenceIntermediateData>
{
public:
    // I share ownership on data
    explicit Sequence(std::shared_ptr<Data> data)
        : m_data(std::move(data))
    {
        initFromData(*m_data);
    }

    ~Sequence() { }

    // I "maybe" share an ownership so pass by const ref
    void bindRoot(const std::shared_ptr<ModelWrapper>& wrapper)
    {
        bind<0, ModelObjectInstance>(wrapper);
    }

    template <size_t Idx, typename ObjectType, typename WrapperType>
    void bind(const std::shared_ptr<WrapperType>& wrapper)
    {
        if (m_objects.size() <= Idx)
        {
            return;
        }
        static_cast<ObjectType&>(*m_objects[Idx]).bind(wrapper);
    }

    SequenceIntermediateData prepareReload() override
    {
        auto data = SequenceIntermediateData {};
        for (auto& o : m_objects)
        {
            data.objects[o->name()] = o->prepareReload();
        }
        return std::move(data);
    }

    void reloadFromData(const SequenceIntermediateData& state, const std::shared_ptr<Data>& data) override
    {
        initFromData(*data);
        for (auto i = 0ul; i < m_objects.size(); ++i)
        {
            // pretend that type is not changed for that name
            auto& name = m_objects[i]->name();

            const auto stateIt = state.objects.find(name);
            if (stateIt != std::end(state.objects))
            {
                m_objects[i]->reloadFromData(stateIt->second, *data->objects[i]);
            }
        }
        // save ref as long as possible to have safe way to ask for object name in destructor for example
        m_data = data;
    }

    void requestReload() override { }

    void reloadDone() override { }

protected:
    void initFromData(const Data& data)
    {
        m_objects.clear();
        m_objects.reserve(data.objects.size());
        for (auto& objectData : data.objects)
        {
            std::unique_ptr<ObjectInstance> instance;
            // DO NOT COPY OBJECT DATA BY ACCIDENT DURING CAST!!!!
            if (const auto d = boost::typeindex::runtime_cast<ModelObjectData*>(objectData.get()))
            {
                // I do allocate on heap here because I want polymorphism
                instance = ObjectFactory::create<ModelObjectInstance>(*d);
            }
            m_objects.push_back(std::move(instance)); // I do not want to use instance in this scope anymore
        }
    }

private:
    std::shared_ptr<Data> m_data;
    std::vector<std::unique_ptr<ObjectInstance>> m_objects;
};

//---------------------------------------------------------------------------------------------------------------------
// Utility
//---------------------------------------------------------------------------------------------------------------------

// some idea to store names of various stuff
class ResourceRegistry
{
    static constexpr size_t REGISTRY_SIZE = 2 >> 10;

    std::array<std::string, REGISTRY_SIZE> resourceNames = {};
};

std::shared_ptr<Data> prepareTestData()
{
    auto data = std::make_shared<Data>();
    // prepair test data
    data->duration = 10.0f;
    auto model = std::make_unique<ModelObjectData>();
    model->name = "Test model";
    model->modelPayload[1] = 255;
    data->objects.push_back(std::move(model));

    // save to file
    {
        auto file = std::fstream {"temp.txt", std::fstream::out | std::fstream::binary};
        boost::archive::binary_oarchive ar {file};
        ar << *data;
    }
    return data;
}

std::shared_ptr<Data> takeDeepCopy(std::shared_ptr<Data> data)
{
    std::stringstream str {};
    boost::archive::binary_oarchive ar {str};
    ar << *data;
    boost::archive::binary_iarchive ari {str};
    auto copy = std::make_shared<Data>();
    ari >> *copy;
    return std::move(copy);
}

//---------------------------------------------------------------------------------------------------------------------
// Usage example
//---------------------------------------------------------------------------------------------------------------------
int main()
{
    std::ios_base::sync_with_stdio(false);
    using namespace std::string_literals;

    // init instance of factory
    FstreamFactory<Data>::instance(std::vector<std::string> {".txt"s});

    prepareTestData();
    std::cout << "Prepared Data!\n";

    // i do not want to save ownership on data, so move it
    auto seq = std::make_unique<Sequence>(FstreamFactory<Data>::load("temp.txt"s));
    auto& m = FstreamFactory<Data>::instance();
    FstreamFactory<Data>::registerUser("temp.txt"s, *seq);
    std::cout << "Registered User!\n";
    {
        auto wrapper = std::make_shared<ModelWrapper>();
        const auto marker = 255;
        wrapper->payload[0] = 1;
        wrapper->payload[2] = marker;
        wrapper->payload[4] = marker;
        wrapper->payload[6] = marker;
        wrapper->payload[8] = marker;
        wrapper->payload[10] = marker;
        wrapper->payload[12] = marker;
        wrapper->payload[14] = marker;
        seq->bindRoot(wrapper);
    }
    std::cout << "Created!\n";
    // test editor routine
    {
        std::cout << "Take data copy:\n";
        auto copy = takeDeepCopy(FstreamFactory<Data>::load("temp.txt"s));
        std::cout << "Copied\n";
        std::cout << "Prepare reload:\n";
        const auto state = seq->prepareReload(); // before modify
        std::cout << "Prepared reload.\n";
        {
            copy->duration = 25;
            auto model = std::make_unique<ModelObjectData>();
            model->name = "Test model2";
            model->modelPayload[0] = 255;
            copy->objects.push_back(std::move(model));
        }
        std::cout << "Added new model to copy\n";
        std::cout << "Reload data for sequence:\n";
        seq->reloadFromData(state, copy); // after modify
        std::cout << "Reloaded data for sequence.\n";
        {
            auto wrapper = std::make_shared<ModelWrapper>();
            const auto marker = 170;
            wrapper->payload[0] = 2;
            wrapper->payload[1] = marker;
            wrapper->payload[3] = marker;
            wrapper->payload[5] = marker;
            wrapper->payload[7] = marker;
            wrapper->payload[9] = marker;
            wrapper->payload[11] = marker;
            wrapper->payload[13] = marker;
            wrapper->payload[15] = marker;
            seq->bind<1, ModelObjectInstance>(wrapper);
        }
        std::cout << "Binded new wrapper to second object.\n";
    }
    std::cout << "Modified!\n";
    std::cout << "Reload sequence from file again:\n";
    // test reload from file
    {
        const auto state = seq->prepareReload(); // before modify
        seq->reloadFromData(state, FstreamFactory<Data>::load("temp.txt"s)); // after modify
    }
    std::cout << "Reloaded!\n";
    std::cout << "Clear sequence!\n";
    seq = nullptr;
    std::cout << "Done!\n";
    return 0;
}
