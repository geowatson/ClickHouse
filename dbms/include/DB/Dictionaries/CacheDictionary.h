#pragma once

#include <DB/Dictionaries/IDictionary.h>
#include <DB/Dictionaries/IDictionarySource.h>
#include <DB/Dictionaries/DictionaryStructure.h>
#include <DB/Common/HashTable/HashMap.h>
#include <DB/Columns/ColumnString.h>
#include <statdaemons/ext/scope_guard.hpp>
#include <Poco/RWLock.h>
#include <cmath>
#include <chrono>
#include <vector>
#include <map>
#include <tuple>

namespace DB
{

class CacheDictionary final : public IDictionary
{
public:
	CacheDictionary(const std::string & name, const DictionaryStructure & dict_struct,
		DictionarySourcePtr source_ptr, const DictionaryLifetime dict_lifetime,
		const std::size_t size)
		: name{name}, dict_struct(dict_struct),
		  source_ptr{std::move(source_ptr)}, dict_lifetime(dict_lifetime),
		  size{round_up_to_power_of_two(size)},
		  cells{this->size}
	{
		if (!this->source_ptr->supportsSelectiveLoad())
			throw Exception{
				"Source cannot be used with CacheDictionary",
				ErrorCodes::UNSUPPORTED_METHOD
			};

		createAttributes();
	}

	CacheDictionary(const CacheDictionary & other)
		: CacheDictionary{other.name, other.dict_struct, other.source_ptr->clone(), other.dict_lifetime, other.size}
	{}

	std::string getName() const override { return name; }

	std::string getTypeName() const override { return "CacheDictionary"; }

	bool isCached() const override { return true; }

	DictionaryPtr clone() const override { return std::make_unique<CacheDictionary>(*this); }

	const IDictionarySource * getSource() const override { return source_ptr.get(); }

	const DictionaryLifetime & getLifetime() const override { return dict_lifetime; }

	bool hasHierarchy() const override { return false; }

	id_t toParent(const id_t id) const override { return 0; }

#define DECLARE_INDIVIDUAL_GETTER(TYPE, LC_TYPE) \
	TYPE get##TYPE(const std::string & attribute_name, const id_t id) const override\
    {\
		const auto idx = getAttributeIndex(attribute_name);\
		const auto & attribute = attributes[idx];\
		if (attribute.type != AttributeType::LC_TYPE)\
			throw Exception{\
				"Type mismatch: attribute " + attribute_name + " has type " + toString(attribute.type),\
				ErrorCodes::TYPE_MISMATCH\
			};\
		\
		PODArray<UInt64> ids{1, id};\
		PODArray<TYPE> out{1};\
		getItems<TYPE>(idx, ids, out);\
        return out.front();\
	}
	DECLARE_INDIVIDUAL_GETTER(UInt8, uint8)
	DECLARE_INDIVIDUAL_GETTER(UInt16, uint16)
	DECLARE_INDIVIDUAL_GETTER(UInt32, uint32)
	DECLARE_INDIVIDUAL_GETTER(UInt64, uint64)
	DECLARE_INDIVIDUAL_GETTER(Int8, int8)
	DECLARE_INDIVIDUAL_GETTER(Int16, int16)
	DECLARE_INDIVIDUAL_GETTER(Int32, int32)
	DECLARE_INDIVIDUAL_GETTER(Int64, int64)
	DECLARE_INDIVIDUAL_GETTER(Float32, float32)
	DECLARE_INDIVIDUAL_GETTER(Float64, float64)
#undef DECLARE_INDIVIDUAL_GETTER
	String getString(const std::string & attribute_name, const id_t id) const override
	{
		const auto idx = getAttributeIndex(attribute_name);
		const auto & attribute = attributes[idx];
		if (attribute.type != AttributeType::string)
			throw Exception{
				"Type mismatch: attribute " + attribute_name + " has type " + toString(attribute.type),
				ErrorCodes::TYPE_MISMATCH
			};

		PODArray<UInt64> ids{1, id};
		ColumnString out;
		getItems(idx, ids, &out);

        return out.getDataAt(0);
	}

#define DECLARE_MULTIPLE_GETTER(TYPE, LC_TYPE)\
	void get##TYPE(const std::string & attribute_name, const PODArray<id_t> & ids, PODArray<TYPE> & out) const override\
	{\
		const auto idx = getAttributeIndex(attribute_name);\
		const auto & attribute = attributes[idx];\
		if (attribute.type != AttributeType::LC_TYPE)\
			throw Exception{\
				"Type mismatch: attribute " + attribute_name + " has type " + toString(attribute.type),\
				ErrorCodes::TYPE_MISMATCH\
			};\
		\
		getItems<TYPE>(idx, ids, out);\
	}
	DECLARE_MULTIPLE_GETTER(UInt8, uint8)
	DECLARE_MULTIPLE_GETTER(UInt16, uint16)
	DECLARE_MULTIPLE_GETTER(UInt32, uint32)
	DECLARE_MULTIPLE_GETTER(UInt64, uint64)
	DECLARE_MULTIPLE_GETTER(Int8, int8)
	DECLARE_MULTIPLE_GETTER(Int16, int16)
	DECLARE_MULTIPLE_GETTER(Int32, int32)
	DECLARE_MULTIPLE_GETTER(Int64, int64)
	DECLARE_MULTIPLE_GETTER(Float32, float32)
	DECLARE_MULTIPLE_GETTER(Float64, float64)
#undef DECLARE_MULTIPLE_GETTER
	void getString(const std::string & attribute_name, const PODArray<id_t> & ids, ColumnString * out) const override
	{
		const auto idx = getAttributeIndex(attribute_name);
		const auto & attribute = attributes[idx];
		if (attribute.type != AttributeType::string)
			throw Exception{
				"Type mismatch: attribute " + attribute_name + " has type " + toString(attribute.type),
				ErrorCodes::TYPE_MISMATCH
			};

		getItems(idx, ids, out);
	}

private:
	struct cell_metadata_t final
	{
		std::uint64_t id;
		std::chrono::system_clock::time_point expires_at;
	};

	struct attribute_t final
	{
		AttributeType type;
		std::tuple<UInt8, UInt16, UInt32, UInt64,
			Int8, Int16, Int32, Int64,
			Float32, Float64,
			String> null_values;
		std::tuple<std::unique_ptr<UInt8[]>,
			std::unique_ptr<UInt16[]>,
			std::unique_ptr<UInt32[]>,
			std::unique_ptr<UInt64[]>,
			std::unique_ptr<Int8[]>,
			std::unique_ptr<Int16[]>,
			std::unique_ptr<Int32[]>,
			std::unique_ptr<Int64[]>,
			std::unique_ptr<Float32[]>,
			std::unique_ptr<Float64[]>,
			std::unique_ptr<StringRef[]>> arrays;
	};

	void createAttributes()
	{
		const auto size = dict_struct.attributes.size();
		attributes.reserve(size);

		for (const auto & attribute : dict_struct.attributes)
		{
			attribute_index_by_name.emplace(attribute.name, attributes.size());
			attributes.push_back(createAttributeWithType(getAttributeTypeByName(attribute.type),
				attribute.null_value));

			if (attribute.hierarchical)
				hierarchical_attribute = &attributes.back();
		}
	}

	attribute_t createAttributeWithType(const AttributeType type, const std::string & null_value)
	{
		attribute_t attr{type};

		switch (type)
		{
			case AttributeType::uint8:
				std::get<UInt8>(attr.null_values) = DB::parse<UInt8>(null_value);
				std::get<std::unique_ptr<UInt8[]>>(attr.arrays) = std::make_unique<UInt8[]>(size);
				break;
			case AttributeType::uint16:
				std::get<UInt16>(attr.null_values) = DB::parse<UInt16>(null_value);
				std::get<std::unique_ptr<UInt16[]>>(attr.arrays) = std::make_unique<UInt16[]>(size);
				break;
			case AttributeType::uint32:
				std::get<UInt32>(attr.null_values) = DB::parse<UInt32>(null_value);
				std::get<std::unique_ptr<UInt32[]>>(attr.arrays) = std::make_unique<UInt32[]>(size);
				break;
			case AttributeType::uint64:
				std::get<UInt64>(attr.null_values) = DB::parse<UInt64>(null_value);
				std::get<std::unique_ptr<UInt64[]>>(attr.arrays) = std::make_unique<UInt64[]>(size);
				break;
			case AttributeType::int8:
				std::get<Int8>(attr.null_values) = DB::parse<Int8>(null_value);
				std::get<std::unique_ptr<Int8[]>>(attr.arrays) = std::make_unique<Int8[]>(size);
				break;
			case AttributeType::int16:
				std::get<Int16>(attr.null_values) = DB::parse<Int16>(null_value);
				std::get<std::unique_ptr<Int16[]>>(attr.arrays) = std::make_unique<Int16[]>(size);
				break;
			case AttributeType::int32:
				std::get<Int32>(attr.null_values) = DB::parse<Int32>(null_value);
				std::get<std::unique_ptr<Int32[]>>(attr.arrays) = std::make_unique<Int32[]>(size);
				break;
			case AttributeType::int64:
				std::get<Int64>(attr.null_values) = DB::parse<Int64>(null_value);
				std::get<std::unique_ptr<Int64[]>>(attr.arrays) = std::make_unique<Int64[]>(size);
				break;
			case AttributeType::float32:
				std::get<Float32>(attr.null_values) = DB::parse<Float32>(null_value);
				std::get<std::unique_ptr<Float32[]>>(attr.arrays) = std::make_unique<Float32[]>(size);
				break;
			case AttributeType::float64:
				std::get<Float64>(attr.null_values) = DB::parse<Float64>(null_value);
				std::get<std::unique_ptr<Float64[]>>(attr.arrays) = std::make_unique<Float64[]>(size);
				break;
			case AttributeType::string:
				std::get<String>(attr.null_values) = null_value;
				std::get<std::unique_ptr<StringRef[]>>(attr.arrays) = std::make_unique<StringRef[]>(size);
				break;
		}

		return attr;
	}

	static bool hasTimeExpired(const std::chrono::system_clock::time_point & time_point)
	{
		return std::chrono::system_clock::now() >= time_point;
	}

	template <typename T>
	void getItems(const std::size_t attribute_idx, const PODArray<id_t> & ids, PODArray<T> & out) const
	{
		HashMap<id_t, std::vector<std::size_t>> outdated_ids;
		auto & attribute = attributes[attribute_idx];
		auto & attribute_array = std::get<std::unique_ptr<T[]>>(attribute.arrays);

		{
			const Poco::ScopedReadRWLock read_lock{rw_lock};
			/// fetch up-to-date values, decide which ones require update
			for (const auto i : ext::range(0, ids.size()))
			{
				const auto id = ids[i];
				if (id == 0)
				{
					out[i] = std::get<T>(attribute.null_values);
					continue;
				}

				const auto cell_idx = getCellIdx(id);
				const auto & cell = cells[cell_idx];

				if (cell.id != id || hasTimeExpired(cell.expires_at))
				{
					out[i] = std::get<T>(attribute.null_values);
					outdated_ids[id].push_back(i);
				}
				else
					out[i] = attribute_array[cell_idx];
			}
		}

		if (outdated_ids.empty())
			return;

		/// request new values
		std::vector<id_t> required_ids(outdated_ids.size());
		std::transform(std::begin(outdated_ids), std::end(outdated_ids), std::begin(required_ids),
			[] (auto & pair) { return pair.first; });

		update(required_ids, [&] (const auto id, const auto cell_idx) {
			const auto attribute_value = attribute_array[cell_idx];

			/// set missing values to out
			for (const auto out_idx : outdated_ids[id])
				out[out_idx] = attribute_value;
		});
	}

	void getItems(const std::size_t attribute_idx, const PODArray<id_t> & ids, ColumnString * out) const
	{
		/// save on some allocations
		out->getOffsets().reserve(ids.size());

		auto & attribute = attributes[attribute_idx];
		auto & attribute_array = std::get<std::unique_ptr<StringRef[]>>(attribute.arrays);

		auto found_outdated_values = false;

		/// perform optimistic version, fallback to pessimistic if failed
		{
			const Poco::ScopedReadRWLock read_lock{rw_lock};

			/// fetch up-to-date values, discard on fail
			for (const auto i : ext::range(0, ids.size()))
			{
				const auto id = ids[i];
				if (id == 0)
				{
					const auto & string = std::get<String>(attribute.null_values);
					out->insertData(string.data(), string.size());
					continue;
				}

				const auto cell_idx = getCellIdx(id);
				const auto & cell = cells[cell_idx];

				if (cell.id != id || hasTimeExpired(cell.expires_at))
				{
					found_outdated_values = true;
					break;
				}
				else
				{
					const auto string_ref = attribute_array[cell_idx];
					out->insertData(string_ref.data, string_ref.size);
				}
			}
		}

		/// optimistic code completed successfully
		if (!found_outdated_values)
			return;

		/// now onto the pessimistic one, discard possibly partial results from the optimistic path
		out->getChars().resize_assume_reserved(0);
		out->getOffsets().resize_assume_reserved(0);

		/// outdated ids joined number of times they've been requested
		HashMap<id_t, std::size_t> outdated_ids;
		/// we are going to store every string separately
		HashMap<id_t, String> map;

		std::size_t total_length = 0;
		{
			const Poco::ScopedReadRWLock read_lock{rw_lock};

			for (const auto i : ext::range(0, ids.size()))
			{
				const auto id = ids[i];
				if (id == 0)
				{
					total_length += 1;
					continue;
				}

				const auto cell_idx = getCellIdx(id);
				const auto & cell = cells[cell_idx];

				if (cell.id != id || hasTimeExpired(cell.expires_at))
					outdated_ids[id] += 1;
				else
				{
					const auto string_ref = attribute_array[cell_idx];
					map[id] = string_ref;
					total_length += string_ref.size + 1;
				};
			}
		}

		/// request new values
		if (!outdated_ids.empty())
		{
			std::vector<id_t> required_ids(outdated_ids.size());
			std::transform(std::begin(outdated_ids), std::end(outdated_ids), std::begin(required_ids),
				[] (auto & pair) { return pair.first; });

			update(required_ids, [&] (const auto id, const auto cell_idx) {
				const auto attribute_value = attribute_array[cell_idx];

				map[id] = attribute_value;
				total_length += attribute_value.size + 1;
			});
		}

		out->getChars().reserve(total_length);

		for (const auto id : ids)
		{
			const auto it = map.find(id);
			const auto string = it != map.end() ? it->second : std::get<String>(attributes[attribute_idx].null_values);
			out->insertData(string.data(), string.size());
		}
	}

	template <typename F>
	void update(const std::vector<id_t> ids, F && on_cell_updated) const
	{
		auto stream = source_ptr->loadIds(ids);
		stream->readPrefix();

		const Poco::ScopedWriteRWLock write_lock{rw_lock};

		while (const auto block = stream->read())
		{
			const auto id_column = typeid_cast<const ColumnVector<UInt64> *>(block.getByPosition(0).column.get());
			if (!id_column)
				throw Exception{
					"Id column has type different from UInt64.",
					ErrorCodes::TYPE_MISMATCH
				};

			const auto & ids = id_column->getData();

			for (const auto i : ext::range(0, ids.size()))
			{
				const auto id = ids[i];
				const auto & cell_idx = getCellIdx(id);
				auto & cell = cells[cell_idx];

				for (const auto attribute_idx : ext::range(0, attributes.size()))
				{
					const auto & attribute_column = *block.getByPosition(attribute_idx + 1).column;
					auto & attribute = attributes[attribute_idx];

					setAttributeValue(attribute, cell_idx, attribute_column[i]);
				}

				std::uniform_int_distribution<std::uint64_t> distribution{
					dict_lifetime.min_sec,
					dict_lifetime.max_sec
				};

				cell.id = id;
				cell.expires_at = std::chrono::system_clock::now() + std::chrono::seconds{distribution(rnd_engine)};

				on_cell_updated(id, cell_idx);
			}
		}

		stream->readSuffix();
	}

	std::uint64_t getCellIdx(const id_t id) const
	{
		const auto hash = intHash64(id);
		const auto idx = hash & (size - 1);
		return idx;
	}

	void setAttributeValue(attribute_t & attribute, const id_t idx, const Field & value) const
	{
		switch (attribute.type)
		{
			case AttributeType::uint8: std::get<std::unique_ptr<UInt8[]>>(attribute.arrays)[idx] = value.get<UInt64>(); break;
			case AttributeType::uint16: std::get<std::unique_ptr<UInt16[]>>(attribute.arrays)[idx] = value.get<UInt64>(); break;
			case AttributeType::uint32: std::get<std::unique_ptr<UInt32[]>>(attribute.arrays)[idx] = value.get<UInt64>(); break;
			case AttributeType::uint64: std::get<std::unique_ptr<UInt64[]>>(attribute.arrays)[idx] = value.get<UInt64>(); break;
			case AttributeType::int8: std::get<std::unique_ptr<Int8[]>>(attribute.arrays)[idx] = value.get<Int64>(); break;
			case AttributeType::int16: std::get<std::unique_ptr<Int16[]>>(attribute.arrays)[idx] = value.get<Int64>(); break;
			case AttributeType::int32: std::get<std::unique_ptr<Int32[]>>(attribute.arrays)[idx] = value.get<Int64>(); break;
			case AttributeType::int64: std::get<std::unique_ptr<Int64[]>>(attribute.arrays)[idx] = value.get<Int64>(); break;
			case AttributeType::float32: std::get<std::unique_ptr<Float32[]>>(attribute.arrays)[idx] = value.get<Float64>(); break;
			case AttributeType::float64: std::get<std::unique_ptr<Float64[]>>(attribute.arrays)[idx] = value.get<Float64>(); break;
			case AttributeType::string:
			{
				const auto & string = value.get<String>();
				auto & string_ref = std::get<std::unique_ptr<StringRef[]>>(attribute.arrays)[idx];
				if (string_ref.data)
					delete[] string_ref.data;

				const auto size = string.size();
				if (size > 0)
				{
					const auto string_ptr = new char[size + 1];
					std::copy(string.data(), string.data() + size + 1, string_ptr);
					string_ref = StringRef{string_ptr, size};
				}
				else
					string_ref = {};

				break;
			}
		}
	}

	std::size_t getAttributeIndex(const std::string & attribute_name) const
	{
		const auto it = attribute_index_by_name.find(attribute_name);
		if (it == std::end(attribute_index_by_name))
			throw Exception{
				"No such attribute '" + attribute_name + "'",
				ErrorCodes::BAD_ARGUMENTS
			};

		return it->second;
	}

	static std::size_t round_up_to_power_of_two(std::size_t n)
	{
		--n;
		n |= n >> 1;
		n |= n >> 2;
		n |= n >> 4;
		n |= n >> 8;
		n |= n >> 16;
		n |= n >> 32;
		++n;

		return n;
	}

	static std::uint64_t getSeed()
	{
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return ts.tv_nsec ^ getpid();
	}

	const std::string name;
	const DictionaryStructure dict_struct;
	const DictionarySourcePtr source_ptr;
	const DictionaryLifetime dict_lifetime;

	mutable Poco::RWLock rw_lock;
	const std::size_t size;
	std::map<std::string, std::size_t> attribute_index_by_name;
	mutable std::vector<attribute_t> attributes;
	mutable std::vector<cell_metadata_t> cells;
	const attribute_t * hierarchical_attribute = nullptr;

	mutable std::mt19937_64 rnd_engine{getSeed()};
};

}
