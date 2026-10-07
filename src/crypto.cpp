#include "roc/vehicle/crypto.hpp"

#include <openssl/evp.h>

#include <array>
#include <memory>
#include <string>

namespace roc::vehicle {

Result<std::string> sha256_hex(std::string_view bytes) {
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context context{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
        return Error{ErrorDomain::internal, ErrorCode::internal_failure, false,
                     "unable to initialize SHA-256"};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 ||
        digest_size != 32U) {
        return Error{ErrorDomain::internal, ErrorCode::internal_failure, false,
                     "unable to finalize SHA-256"};
    }
    static constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned int index = 0; index < digest_size; ++index) {
        const auto byte = digest[index];
        result.push_back(hexadecimal[(byte >> 4U) & 0x0fU]);
        result.push_back(hexadecimal[byte & 0x0fU]);
    }
    return result;
}

}  // namespace roc::vehicle
