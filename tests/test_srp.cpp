// SRP6a challenge/proof tests
#include <catch_amalgamated.hpp>
#include "auth/srp.hpp"
#include "auth/crypto.hpp"
#include <stdexcept>

using wowee::auth::SRP;
using wowee::auth::Crypto;

// WoW 3.3.5a uses well-known SRP6a parameters.
// Generator g = 7, N = a large 32-byte safe prime.
// We use the canonical WoW values for integration-level tests.

static const std::vector<uint8_t> kWoWGenerator = { 7 };

// WoW's 32-byte large safe prime (little-endian)
static const std::vector<uint8_t> kWoWPrime = {
    0xB7, 0x9B, 0x3E, 0x2A, 0x87, 0x82, 0x3C, 0xAB,
    0x8F, 0x5E, 0xBF, 0xBF, 0x8E, 0xB1, 0x01, 0x08,
    0x53, 0x50, 0x06, 0x29, 0x8B, 0x5B, 0xAD, 0xBD,
    0x5B, 0x53, 0xE1, 0x89, 0x5E, 0x64, 0x4B, 0x89
};

TEST_CASE("SRP agrees with server for zero-padded public values", "[srp]") {
    using wowee::auth::BigNum;
    const BigNum modulus(kWoWPrime), generator(7);
    const std::string username = "SYNTHETIC";
    std::vector<uint8_t> salt(32, 0x31);
    salt.back() = 0; // A wire salt remains 32 bytes even when its integer is shorter.
    auto saltedHash = salt;
    const auto identity = Crypto::sha1(username + ":TESTPASSWORD");
    saltedHash.insert(saltedHash.end(), identity.begin(), identity.end());
    const BigNum verifier = generator.modPow(BigNum(Crypto::sha1(saltedHash)), modulus);

    // Find a reproducible server ephemeral whose wire value ends in a zero byte.
    // Client randomness cannot change this required 32-byte B hash input.
    BigNum serverPrivate, serverPublic;
    for (uint32_t candidate = 1; candidate <= 4096; ++candidate) {
        serverPrivate = BigNum(candidate);
        serverPublic = verifier.multiply(BigNum(3)).add(
            generator.modPow(serverPrivate, modulus)).mod(modulus);
        if (serverPublic.toArray(true).size() < 32) break;
    }
    REQUIRE(serverPublic.toArray(true).size() < 32);
    SRP client;
    client.initialize(username, "TESTPASSWORD");
    client.feed(serverPublic.toArray(true, 32), kWoWGenerator, kWoWPrime, salt);

    auto scrambleInput = client.getA();
    const auto publicBytes = serverPublic.toArray(true, 32);
    scrambleInput.insert(scrambleInput.end(), publicBytes.begin(), publicBytes.end());
    const BigNum scramble(Crypto::sha1(scrambleInput));
    const auto secret = BigNum(client.getA()).multiply(verifier.modPow(scramble, modulus))
        .modPow(serverPrivate, modulus).toArray(true, 32);
    size_t first = 0;
    while (first < secret.size() && secret[first] == 0) ++first;
    first += (secret.size() - first) % 2;
    std::vector<uint8_t> even, odd;
    for (size_t i = first; i < secret.size(); i += 2) {
        even.push_back(secret[i]);
        odd.push_back(secret[i + 1]);
    }
    const auto evenHash = Crypto::sha1(even), oddHash = Crypto::sha1(odd);
    std::vector<uint8_t> key;
    for (size_t i = 0; i < evenHash.size(); ++i) {
        key.push_back(evenHash[i]);
        key.push_back(oddHash[i]);
    }
    CHECK(client.getSessionKey() == key);

    auto proofInput = Crypto::sha1(kWoWPrime);
    const auto generatorHash = Crypto::sha1(kWoWGenerator);
    for (size_t i = 0; i < proofInput.size(); ++i) proofInput[i] ^= generatorHash[i];
    for (const auto& part : {Crypto::sha1(username), salt, client.getA(), publicBytes, key})
        proofInput.insert(proofInput.end(), part.begin(), part.end());
    const auto proof = Crypto::sha1(proofInput);
    CHECK(client.getM1() == proof);
    auto serverProof = client.getA();
    serverProof.insert(serverProof.end(), proof.begin(), proof.end());
    serverProof.insert(serverProof.end(), key.begin(), key.end());
    CHECK(client.verifyServerProof(Crypto::sha1(serverProof)));
}

TEST_CASE("SRP interleave trims zero prefix and pairs remaining bytes", "[srp]") {
    const std::vector<std::pair<size_t, std::string>> vectors{
        {0, "ed2976c475109d044df3443c9cc86c31077e9c7db8e04d3d699f68bd6bcc6c6b20df81f6b7d66fa0"},
        {1, "324ee42a4dc3379278dea177486fba3715c1d9173be0101572fd5f0cc9b6708075b5b8928c829c52"},
        {2, "324ee42a4dc3379278dea177486fba3715c1d9173be0101572fd5f0cc9b6708075b5b8928c829c52"},
        {3, "249481298dd8dc043490ee550d33ace31ca8ba9aa8a8e322fdeb1c6f2c27e8622184c4131a7916b1"},
        {32, "dada3939a3a3eeee5e5e6b6b4b4b0d0d32325555bfbfefef9595606018189090afafd8d807070909"},
    };
    // Fixed SHA-1 vectors independently generated with Python hashlib.
    for (const auto& [zeros, expected] : vectors) {
        CAPTURE(zeros);
        std::vector<uint8_t> secret;
        for (size_t i = 0; i < 32; ++i)
            secret.push_back(i < zeros ? 0 : static_cast<uint8_t>(i + 1));
        const auto bytes = SRP::interleaveSessionKey(secret);
        std::string hex;
        for (const auto byte : bytes) {
            hex += "0123456789abcdef"[byte >> 4];
            hex += "0123456789abcdef"[byte & 15];
        }
        CHECK(hex == expected);
    }
    CHECK_THROWS_AS(SRP::interleaveSessionKey(std::vector<uint8_t>(31)), std::invalid_argument);
}

TEST_CASE("SRP initialize stores credentials", "[srp]") {
    SRP srp;
    // Should not throw
    REQUIRE_NOTHROW(srp.initialize("TEST", "PASSWORD"));
}

TEST_CASE("SRP initializeWithHash accepts pre-computed hash", "[srp]") {
    // Pre-compute SHA1("TEST:PASSWORD")
    auto hash = Crypto::sha1(std::string("TEST:PASSWORD"));
    REQUIRE(hash.size() == 20);

    SRP srp;
    REQUIRE_NOTHROW(srp.initializeWithHash("TEST", hash));
}

TEST_CASE("SRP feed produces A and M1 of correct sizes", "[srp]") {
    SRP srp;
    srp.initialize("TEST", "PASSWORD");

    // Fabricate a server B (32 bytes, non-zero to avoid SRP abort)
    std::vector<uint8_t> B(32, 0);
    B[0] = 0x42; // Non-zero

    std::vector<uint8_t> salt(32, 0xAA);

    srp.feed(B, kWoWGenerator, kWoWPrime, salt);

    auto A = srp.getA();
    auto M1 = srp.getM1();
    auto K = srp.getSessionKey();

    // A should be 32 bytes (same size as N)
    REQUIRE(A.size() == 32);
    // M1 is SHA1 → 20 bytes
    REQUIRE(M1.size() == 20);
    // K is the interleaved session key → 40 bytes
    REQUIRE(K.size() == 40);
}

TEST_CASE("SRP A is non-zero", "[srp]") {
    SRP srp;
    srp.initialize("PLAYER", "SECRET");

    std::vector<uint8_t> B(32, 0);
    B[3] = 0x01;
    std::vector<uint8_t> salt(32, 0xBB);

    srp.feed(B, kWoWGenerator, kWoWPrime, salt);

    auto A = srp.getA();
    bool allZero = true;
    for (auto b : A) {
        if (b != 0) { allZero = false; break; }
    }
    REQUIRE_FALSE(allZero);
}

TEST_CASE("SRP different passwords produce different M1", "[srp]") {
    auto runSrp = [](const std::string& pass) {
        SRP srp;
        srp.initialize("TESTUSER", pass);
        std::vector<uint8_t> B(32, 0);
        B[0] = 0x11;
        std::vector<uint8_t> salt(32, 0xCC);
        srp.feed(B, kWoWGenerator, kWoWPrime, salt);
        return srp.getM1();
    };

    auto m1a = runSrp("PASSWORD1");
    auto m1b = runSrp("PASSWORD2");
    REQUIRE(m1a != m1b);
}

TEST_CASE("SRP verifyServerProof rejects wrong proof", "[srp]") {
    SRP srp;
    srp.initialize("TEST", "PASSWORD");

    std::vector<uint8_t> B(32, 0);
    B[0] = 0x55;
    std::vector<uint8_t> salt(32, 0xDD);

    srp.feed(B, kWoWGenerator, kWoWPrime, salt);

    // Random 20 bytes should not match the expected M2
    std::vector<uint8_t> fakeM2(20, 0xFF);
    REQUIRE_FALSE(srp.verifyServerProof(fakeM2));
}

TEST_CASE("SRP setUseHashedK changes behavior", "[srp]") {
    auto runWithHashedK = [](bool useHashed) {
        SRP srp;
        srp.setUseHashedK(useHashed);
        srp.initialize("TEST", "PASSWORD");
        std::vector<uint8_t> B(32, 0);
        B[0] = 0x22;
        std::vector<uint8_t> salt(32, 0xEE);
        srp.feed(B, kWoWGenerator, kWoWPrime, salt);
        return srp.getM1();
    };

    auto m1_default = runWithHashedK(false);
    auto m1_hashed = runWithHashedK(true);
    // Different k derivation → different M1
    REQUIRE(m1_default != m1_hashed);
}
