#include <windows.h>
#include <bcrypt.h>

#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

std::string HexEncode(const UCHAR* bytes, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    result[i * 2] = kHex[(bytes[i] >> 4) & 0x0f];
    result[i * 2 + 1] = kHex[bytes[i] & 0x0f];
  }
  return result;
}

bool HmacSha256Hex(
    const std::string& key,
    const std::string& message,
    std::string* digest_hex) {
  if (!digest_hex || key.empty() ||
      key.size() > std::numeric_limits<ULONG>::max() ||
      message.size() > std::numeric_limits<ULONG>::max()) {
    return false;
  }

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_length = 0;
  DWORD hash_length = 0;
  DWORD bytes_written = 0;
  std::vector<UCHAR> hash_object;
  std::vector<UCHAR> digest;

  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm,
      BCRYPT_SHA256_ALGORITHM,
      nullptr,
      BCRYPT_ALG_HANDLE_HMAC_FLAG);
  if (!BCRYPT_SUCCESS(status)) return false;

  auto cleanup = [&]() {
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!hash_object.empty()) SecureZeroMemory(hash_object.data(), hash_object.size());
    if (!digest.empty()) SecureZeroMemory(digest.data(), digest.size());
  };

  status = BCryptGetProperty(
      algorithm,
      BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length),
      sizeof(object_length),
      &bytes_written,
      0);
  if (!BCRYPT_SUCCESS(status) || object_length == 0) {
    cleanup();
    return false;
  }
  status = BCryptGetProperty(
      algorithm,
      BCRYPT_HASH_LENGTH,
      reinterpret_cast<PUCHAR>(&hash_length),
      sizeof(hash_length),
      &bytes_written,
      0);
  if (!BCRYPT_SUCCESS(status) || hash_length != 32) {
    cleanup();
    return false;
  }

  hash_object.resize(object_length);
  digest.resize(hash_length);
  status = BCryptCreateHash(
      algorithm,
      &hash,
      hash_object.data(),
      static_cast<ULONG>(hash_object.size()),
      const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(key.data())),
      static_cast<ULONG>(key.size()),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    cleanup();
    return false;
  }
  status = BCryptHashData(
      hash,
      const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(message.data())),
      static_cast<ULONG>(message.size()),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    cleanup();
    return false;
  }
  status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
  if (!BCRYPT_SUCCESS(status)) {
    cleanup();
    return false;
  }

  *digest_hex = HexEncode(digest.data(), digest.size());
  cleanup();
  return true;
}

}  // namespace

int wmain() {
  const std::string access_code = "DeskLink-Test-Access-Code-123!";
  const std::string nonce(64, '0');
  const std::string message =
      "DeskLink access proof v1\n"
      "web-test-01\n"
      "win-test-01\n"
      "session-123\n" +
      nonce;
  const std::string expected =
      "4bda9e0359353d0b47b5e7c22c24664318cbc42dfdcc16a09170e2fd1ea95214";

  std::string actual;
  if (!HmacSha256Hex(access_code, message, &actual)) {
    std::wcerr << L"BCrypt HMAC-SHA256 failed\n";
    return 1;
  }
  if (actual != expected) {
    std::cerr << "Access proof vector mismatch\nexpected: " << expected
              << "\nactual:   " << actual << "\n";
    return 2;
  }

  UCHAR first[32]{};
  UCHAR second[32]{};
  if (!BCRYPT_SUCCESS(BCryptGenRandom(
          nullptr, first, sizeof(first), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ||
      !BCRYPT_SUCCESS(BCryptGenRandom(
          nullptr, second, sizeof(second), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    std::wcerr << L"BCryptGenRandom failed\n";
    return 3;
  }
  bool different = false;
  for (size_t i = 0; i < sizeof(first); ++i) {
    different = different || first[i] != second[i];
  }
  SecureZeroMemory(first, sizeof(first));
  SecureZeroMemory(second, sizeof(second));
  if (!different) {
    std::wcerr << L"CSPRNG smoke produced identical 256-bit nonces\n";
    return 4;
  }

  std::wcout << L"DeskLink access proof BCrypt vector passed.\n";
  return 0;
}
