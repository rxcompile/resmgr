#pragma once

#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

#include <boost/signals2/signal.hpp>

#include <memory>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <unordered_map>

//---------------------------------------------------------------------------------------------------------------------
// Support
//---------------------------------------------------------------------------------------------------------------------
template <class T>
class Singleton
{
public:
    template <typename... Args>
    static T& instance(Args&&... args)
    {
        std::call_once(
            get_once_flag(),
            [](Args&&... args)
            {
                instance_.reset(new T(std::forward<Args>(args)...));
            }, std::forward<Args>(args)...);
        return *instance_.get();
    }

protected:
    explicit Singleton<T>() {}

    ~Singleton<T>() {}

    Singleton(const Singleton&) = delete;

    const Singleton& operator=(const Singleton&) = delete;

private:
    static inline std::unique_ptr<T> instance_ = {};

    static std::once_flag& get_once_flag()
    {
        static std::once_flag once_;
        return once_;
    }
};

//---------------------------------------------------------------------------------------------------------------------
// Resource management
//---------------------------------------------------------------------------------------------------------------------
class IReloadableBase
{
public:
    virtual ~IReloadableBase() = default;

    virtual void requestReload() = 0;

    virtual void reloadDone() = 0;

    // scoped for auto disconnect
    boost::signals2::scoped_connection requestReloadConnection;
    boost::signals2::scoped_connection reloadDoneConnection;
};

template <typename ResourceType, typename IntermediateStateType>
class IReloadable : public IReloadableBase
{
public:
    virtual ~IReloadable() = default;

    virtual IntermediateStateType prepareReload() = 0;

    virtual void reloadFromData(const IntermediateStateType& state, const ResourceType& data) = 0;
};

template <typename Implementation, typename T>
class Factory : public Singleton<Implementation>
{
    // declared somewhere else
    friend class DebugInterface;

public:
    using ValueType = std::decay_t<T>;
    using ResourcePathType = std::string;

    using TypeSharedPtr = std::shared_ptr<ValueType>;
    using TypeWeakPtr = std::weak_ptr<ValueType>;
    using TypeUniquePtr = std::unique_ptr<ValueType>;

    using RequestReloadSignal = boost::signals2::signal<void()>;
    using ReloadDoneSignal = boost::signals2::signal<void()>;
    
    struct Resource
    {
        TypeWeakPtr resource;
        RequestReloadSignal requestReload;
        ReloadDoneSignal reloadDone;
    };

    using CacheType = std::unordered_map<ResourcePathType, Resource>;

    static TypeSharedPtr load(const ResourcePathType& resource)
    {
        return Factory::instance().loadInternal(resource);
    }

    // Send by value to pin resource while saving
    static bool save(const ResourcePathType& resource, TypeSharedPtr data)
    {
        return Factory::instance().saveInternal(resource, std::move(data));
    }

    static void registerUser(
        const ResourcePathType& resource,
        IReloadableBase& user)
    {
        auto& signals = Factory::instance().m_cache;
        auto sig = signals.find(resource);
        if (sig != std::end(signals))
        {
            user.requestReloadConnection = sig->second.requestReload.connect(
                std::bind(&IReloadableBase::requestReload, &user));
            user.reloadDoneConnection = sig->second.reloadDone.connect(
                std::bind(&IReloadableBase::reloadDone, &user));
        }
    }

    virtual ~Factory()
    {
        std::scoped_lock<std::mutex> guard {m_access};
        for (auto& kv : m_cache)
        {
            if (!kv.second.resource.expired())
            {
                std::cout << "ERROR: Resource " << kv.first << " leaked!\n";
            }
        }
    }

protected:
    virtual bool hasValidExtension(std::string_view resource) = 0;

    virtual bool doLoad(std::string_view resource, ValueType& data) = 0;

    virtual bool doSave(std::string_view resource, ValueType& data) = 0;

private:

    TypeSharedPtr loadInternal(const std::string& path)
    {
        if (!hasValidExtension(path))
        {
            return {};
        }

        {
            std::scoped_lock<std::mutex> guard {m_access};
            auto cached = getFromCache(path);
            if (cached)
                return cached;
        }

        auto unique = std::make_unique<ValueType>();

        if (!doLoad(path, *unique))
        {
            // not loaded correctly
            return {};
        }

        {
            std::scoped_lock<std::mutex> guard {m_access};
            // try to find again
            auto cached = getFromCache(path);
            if (cached)
                return cached;

            // steal to shared and put into cache
            auto shared = TypeSharedPtr(unique.release(), destroyData);
            auto res = Resource{};
            res.resource = TypeWeakPtr {shared};
            m_cache[path] = std::move(res);
            return shared; // RNVO should handle moving named shared_ptr
        }
    }

    bool saveInternal(const std::string& path, TypeSharedPtr data)
    {
        if (!hasValidExtension(path))
        {
            return false;
        }

        if (!doSave(path, *data))
        {
            return false;
        }

        std::scoped_lock<std::mutex> guard {m_access};
        // we need somehow notify users about resource changing
        // or else they will crash out application
        auto res = Resource{};
        res.resource = TypeWeakPtr {data};
        m_cache[path] = std::move(res);
        return true;
    }

    TypeSharedPtr getFromCache(const std::string& resource)
    {
        const auto cached = m_cache.find(resource);
        if (cached != end(m_cache))
        {
            if (cached->second.resource.expired())
            {
                // This is unfortunate we should be destoyed by manager itself
                // OK, lets remove it
                m_cache.erase(cached);
                return {};
            }
            return cached->second.resource.lock();
        }
        return {};
    }

    static void destroyData(ValueType* raw)
    {
        auto& manager = Factory::instance();
        std::lock_guard<std::mutex> guard {manager.m_access};
        // first expired is our destroyed pointer
        // we could check if we can get internal control block of weak
        // but its too hackerish so no
        const auto expired = std::find_if(begin(manager.m_cache), end(manager.m_cache), [](auto& p)
        {
            return p.second.resource.expired();
        });
        if (expired == end(manager.m_cache))
        {
            // THIS IS BAD!
            // We shouldn't get destroyed by shared_ptr decref
            // and could not find any expired weak!
            return;
        }
        manager.m_cache.erase(expired);
    }

private:
    CacheType m_cache;
    std::mutex m_access;
};

template <typename T>
class FstreamFactory : public Factory<FstreamFactory<T>, T>
{
    friend class Singleton<FstreamFactory>;

protected:
    FstreamFactory()
        : Factory<FstreamFactory, T>() {}

    explicit FstreamFactory(std::vector<std::string> extensions)
        : Factory<FstreamFactory, T>()
        , m_supportedExtensions(std::move(extensions)) { }


    bool hasValidExtension(std::string_view resource) override
    {
        const auto extension = std::filesystem::path(resource).extension().string();
        return std::find(begin(m_supportedExtensions), end(m_supportedExtensions), extension) != end(
            m_supportedExtensions);
    }

    bool doLoad(std::string_view resource, typename FstreamFactory::ValueType& data) override
    {
        const auto resourcepath = std::string {resource};
        auto file = std::fstream {resourcepath, std::fstream::in | std::fstream::binary};
        if (!file.is_open())
        {
            std::cout << "ERROR: cannot open file!" << resourcepath;
            return false;
        }
        boost::archive::binary_iarchive ar {file};
        ar >> data;
        return true;
    }

    bool doSave(std::string_view resource, typename FstreamFactory::ValueType& data) override
    {
        const auto resourcepath = std::string {resource};
        auto file = std::fstream {resourcepath, std::fstream::out | std::fstream::binary};
        if (!file.is_open())
        {
            std::cout << "ERROR: cannot open file!" << resourcepath;
            return false;
        }
        boost::archive::binary_oarchive ar {file};
        ar << data;
        return true;
    }

private:
    std::vector<std::string> m_supportedExtensions;
};
