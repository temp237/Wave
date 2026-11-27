#include "Encryption.hpp"

std::string Encryption::Sha256(std::string Plaintext)
{
    CryptoPP::SHA256 hash;
    CryptoPP::byte digest[CryptoPP::SHA256::DIGESTSIZE];

    hash.CalculateDigest(digest,
        (const CryptoPP::byte*)Plaintext.data(),
        Plaintext.size());

    std::string output;
    CryptoPP::HexEncoder encoder;
    encoder.Attach(new CryptoPP::StringSink(output));
    encoder.Put(digest, sizeof(digest));
    encoder.MessageEnd();

    return output;
}
