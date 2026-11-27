#pragma once
#include <cryptopp/sha.h>
#include <cryptopp/hex.h>
#include <cryptopp/filters.h>
#include <string>

class Encryption
{
public:
	static std::string Sha256(std::string Plaintext);
};

