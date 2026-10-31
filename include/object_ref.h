#pragma once
#include <Basic.hpp>
#include <CoreUObject_classes.hpp>
#include <cstdint>

// A UObject reference that is safe to keep past the tick it was read on.
//
// A raw UObject* held across ticks can dangle. The object may be garbage
// collected (world travel, an actor or component destroyed) and its memory
// handed to a new object, and writing through the old pointer then corrupts
// whatever lives there now. try/catch does not catch that.
//
// ObjectRef keeps the object's GObjects slot and class next to the pointer.
// Get() hands the pointer back only while that slot still holds the same
// object, of the same class, that has not been marked as garbage or started
// destroying. If a new object of the same class is later created at the same
// address and takes the same slot, Get() returns it: it is a live object of
// the right type, so it is safe to use, but it is not the one that was
// recorded. The slot table's chunks are never freed, so reading the slot is
// safe even after the object is gone, and the object itself is only touched
// once the slot says it is still there.
//
// Why not the SDK's FWeakObjectPtr: its Get() looks up ObjectIndex alone and
// never compares ObjectSerialNumber, so it returns whatever object has taken
// that slot since. A serial check would also need a serial to compare, and
// the engine only assigns one to an object the first time something takes a
// real weak pointer to it; most components never get one.
//
// Game thread only. Get() on the game thread is meant not to race a free, so
// call it there.
template<typename T>
class ObjectRef
{
public:
    ObjectRef() = default;
    explicit ObjectRef(T* obj) { Set(obj); }

    // obj must be live right now (read this tick, on the game thread).
    void Set(T* obj)
    {
        m_obj   = obj;
        m_index = obj ? obj->Index : -1;
        m_class = obj ? obj->Class : nullptr;
    }

    void Reset() { Set(nullptr); }

    // Same reference means the same object recorded in the same slot. This is
    // a plain field compare and does not look at the object, so a dead object
    // still equals its own reference. Compare Get() results to ask about
    // liveness.
    bool operator==(const ObjectRef& other) const
    {
        return m_obj == other.m_obj && m_index == other.m_index && m_class == other.m_class;
    }
    bool operator!=(const ObjectRef& other) const { return !(*this == other); }

    T* Get() const
    {
        if (!m_obj)
            return nullptr;

        if (SDK::UObject::GObjects->GetByIndex(m_index) != m_obj)
            return nullptr;

        if (m_obj->Class != m_class)
            return nullptr;

        constexpr uint32_t kDeadFlags =
            static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed) |
            static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed) |
            static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);
        if (static_cast<uint32_t>(m_obj->Flags) & kDeadFlags)
            return nullptr;

        return m_obj;
    }

private:
    T*                m_obj   = nullptr;
    int32_t           m_index = -1;
    const SDK::UClass* m_class = nullptr;
};
