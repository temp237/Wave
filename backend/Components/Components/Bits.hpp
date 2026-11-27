#pragma once
#include <bit>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

// Bits are packed LSB-first; multi-byte values are little-endian. Matches frontend bits.rs.

class BitWriter
{
private:
	std::vector<uint8_t> buf;
	size_t bit = 0;
public:
	void WriteBits(uint64_t value, uint32_t count)
	{
		for (uint32_t i = 0; i < count; i++) {
			if (bit % 8 == 0)
				buf.push_back(0);
			if ((value >> i) & 1)
				buf.back() |= static_cast<uint8_t>(1u << (bit % 8));
			bit++;
		}
	}

	void WriteBool(bool v) { WriteBits(v ? 1 : 0, 1); }

	template <typename T> requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)
	void Write(T v) { WriteBits(static_cast<std::make_unsigned_t<T>>(v), sizeof(T) * 8); }

	void WriteF32(float v) { Write(std::bit_cast<uint32_t>(v)); }
	void WriteF64(double v) { Write(std::bit_cast<uint64_t>(v)); }

	void WriteString(const std::string& s)
	{
		Write(static_cast<uint32_t>(s.size()));
		for (char c : s)
			Write(static_cast<uint8_t>(c));
	}

	const std::vector<uint8_t>& Data() const { return buf; }
};

class BitReader
{
private:
	const uint8_t* data;
	size_t size;
	size_t bit = 0;
	bool ok = true;
public:
	BitReader(const uint8_t* data, size_t size) : data(data), size(size) {}
	explicit BitReader(const std::string& s) : BitReader(reinterpret_cast<const uint8_t*>(s.data()), s.size()) {}

	uint64_t ReadBits(uint32_t count)
	{
		if (!ok || bit + count > size * 8) {
			ok = false;
			return 0;
		}
		uint64_t v = 0;
		for (uint32_t i = 0; i < count; i++) {
			if ((data[bit / 8] >> (bit % 8)) & 1)
				v |= uint64_t(1) << i;
			bit++;
		}
		return v;
	}

	bool ReadBool() { return ReadBits(1) == 1; }

	template <typename T> requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)
	T Read() { return static_cast<T>(static_cast<std::make_unsigned_t<T>>(ReadBits(sizeof(T) * 8))); }

	float ReadF32() { return std::bit_cast<float>(Read<uint32_t>()); }
	double ReadF64() { return std::bit_cast<double>(Read<uint64_t>()); }

	std::string ReadString()
	{
		uint32_t len = Read<uint32_t>();
		if (!ok || bit + size_t(len) * 8 > size * 8) {
			ok = false;
			return {};
		}
		std::string s(len, '\0');
		for (auto& c : s)
			c = static_cast<char>(Read<uint8_t>());
		return s;
	}

	bool Ok() const { return ok; }
};
