#pragma once
// Stand-in for the Dumper-7 Basic.hpp and the parts of CoreUObject and the
// container header that include/plugin_map.h touches, so the tests build
// without the game SDK. Names, members and signatures match the real SDK.
#include <cstdint>
#include <string>
#include <vector>

namespace SDK
{
	using int32 = int32_t;
	using int64 = int64_t;
	using uint32 = uint32_t;
	using uint64 = uint64_t;

	namespace Offsets
	{
		constexpr int32 GObjects = 0;
		constexpr int32 AppendString = 0;
	}

	namespace InSDKUtils
	{
		inline uintptr_t GetImageBase() { return 0; }
	}

	enum class EClassCastFlags : uint64
	{
		None = 0x0000000000000000,
		Enum = 0x0000000000000004,
		Struct = 0x0000000000000010,
	};

	// A game TArray: the data pointer, then Num and Max.
	template <typename ArrayElementType>
	class TArray
	{
	protected:
		ArrayElementType* Data = nullptr;
		int32 NumElements = 0;
		int32 MaxElements = 0;

	public:
		TArray() = default;
		TArray(ArrayElementType* data, int32 num) : Data(data), NumElements(num), MaxElements(num) {}

		inline int32 Num() const { return NumElements; }
		inline const ArrayElementType* GetDataPtr() const { return Data; }
		inline bool IsValidIndex(int32 Index) const { return Data && Index >= 0 && Index < NumElements; }
		inline ArrayElementType& operator[](int32 Index) { return Data[Index]; }
		inline const ArrayElementType& operator[](int32 Index) const { return Data[Index]; }
	};

	template <typename KeyType, typename ValueType>
	class TPair
	{
	public:
		KeyType First;
		ValueType Second;
		TPair(KeyType Key, ValueType Value) : First(Key), Second(Value) {}
	};

	// The name table behind FName::ToString.
	class FName
	{
	public:
		int32  ComparisonIndex = 0;
		uint32 Number = 0;

		FName() = default;
		explicit FName(const std::string& text)
		{
			auto& table = Table();
			for (size_t i = 0; i < table.size(); ++i)
				if (table[i] == text)
				{
					ComparisonIndex = static_cast<int32>(i);
					return;
				}
			table.push_back(text);
			ComparisonIndex = static_cast<int32>(table.size() - 1);
		}

		std::string ToString() const { return Table()[ComparisonIndex]; }

		static void InitManually(void*) {}

	private:
		static std::vector<std::string>& Table()
		{
			static std::vector<std::string> table{ "None" };
			return table;
		}
	};

	class UClass;
	class UObject;

	class TUObjectArrayWrapper
	{
	public:
		std::vector<UObject*> Objects;
		void InitManually(void*) {}
	};

	class UObject
	{
	public:
		FName           Name;
		EClassCastFlags CastFlags = EClassCastFlags::None;

		static inline TUObjectArrayWrapper GObjects;

		std::string GetName() const { return Name.ToString(); }
		bool HasTypeFlag(EClassCastFlags Required) const
		{
			return Required == EClassCastFlags::None
				|| (static_cast<uint64>(CastFlags) & static_cast<uint64>(Required)) != 0;
		}

		// Walks every object, like the SDK's FindObjectFastImpl.
		template <typename UEType = UObject>
		static UEType* FindObjectFast(const std::string& Name, EClassCastFlags RequiredType = EClassCastFlags::None)
		{
			for (UObject* Object : GObjects.Objects)
				if (Object && Object->HasTypeFlag(RequiredType) && Object->GetName() == Name)
					return static_cast<UEType*>(Object);
			return nullptr;
		}
	};

	class UEnum : public UObject
	{
	public:
		TArray<TPair<FName, int64>> Names;
	};
}
