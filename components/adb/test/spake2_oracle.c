/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * BoringSSL's side of adb pairing for the crate's tests, in hex over stdio:
 *   spake2 alice|bob <password>   prints its message, reads the peer's, prints the key
 *   seal|open <key material> <sequence> <data>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/aead.h>
#include <openssl/curve25519.h>
#include <openssl/digest.h>
#include <openssl/hkdf.h>

static const uint8_t client_name[] = "adb pair client";
static const uint8_t server_name[] = "adb pair server";

static size_t unhex(const char *hex, uint8_t *out, size_t max) {
    size_t n = strlen(hex) / 2;
    if (n > max) {
        exit(2);
    }
    for (size_t i = 0; i < n; i++) {
        sscanf(hex + 2 * i, "%2hhx", &out[i]);
    }
    return n;
}

static void put_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
    fflush(stdout);
}

static int spake2(int alice, const char *password_hex) {
    uint8_t password[256];
    size_t password_len = unhex(password_hex, password, sizeof(password));
    const uint8_t *mine = alice ? client_name : server_name;
    const uint8_t *theirs = alice ? server_name : client_name;
    SPAKE2_CTX *ctx = SPAKE2_CTX_new(alice ? spake2_role_alice : spake2_role_bob, mine, sizeof(client_name), theirs, sizeof(server_name));
    uint8_t msg[SPAKE2_MAX_MSG_SIZE];
    size_t msg_len;
    if (!ctx || !SPAKE2_generate_msg(ctx, msg, &msg_len, sizeof(msg), password, password_len)) {
        return 1;
    }
    put_hex(msg, msg_len);
    char line[256];
    if (!fgets(line, sizeof(line), stdin)) {
        return 1;
    }
    line[strcspn(line, "\n")] = 0;
    uint8_t peer[SPAKE2_MAX_MSG_SIZE];
    size_t peer_len = unhex(line, peer, sizeof(peer));
    uint8_t key[SPAKE2_MAX_KEY_SIZE];
    size_t key_len;
    if (!SPAKE2_process_msg(ctx, key, &key_len, sizeof(key), peer, peer_len)) {
        printf("FAIL\n");
        return 0;
    }
    put_hex(key, key_len);
    SPAKE2_CTX_free(ctx);
    return 0;
}

static int record(int seal, const char *material_hex, const char *sequence, const char *data_hex) {
    static uint8_t material[64], data[2 * 8192 + 16], out[2 * 8192 + 32];
    size_t material_len = unhex(material_hex, material, sizeof(material));
    size_t data_len = unhex(data_hex, data, sizeof(data));
    static const char info[] = "adb pairing_auth aes-128-gcm key";
    uint8_t key[16];
    if (!HKDF(key, sizeof(key), EVP_sha256(), material, material_len, NULL, 0, (const uint8_t *)info, sizeof(info) - 1)) {
        return 1;
    }
    EVP_AEAD_CTX ctx;
    if (!EVP_AEAD_CTX_init(&ctx, EVP_aead_aes_128_gcm(), key, sizeof(key), EVP_AEAD_DEFAULT_TAG_LENGTH, NULL)) {
        return 1;
    }
    uint8_t nonce[12] = {0};
    uint64_t seq = strtoull(sequence, NULL, 10);
    memcpy(nonce, &seq, sizeof(seq));
    size_t out_len;
    int ok = seal ? EVP_AEAD_CTX_seal(&ctx, out, &out_len, sizeof(out), nonce, sizeof(nonce), data, data_len, NULL, 0)
                  : EVP_AEAD_CTX_open(&ctx, out, &out_len, sizeof(out), nonce, sizeof(nonce), data, data_len, NULL, 0);
    if (ok) {
        put_hex(out, out_len);
    } else {
        printf("FAIL\n");
    }
    EVP_AEAD_CTX_cleanup(&ctx);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "spake2")) {
        return spake2(!strcmp(argv[2], "alice"), argv[3]);
    }
    if (argc == 5 && (!strcmp(argv[1], "seal") || !strcmp(argv[1], "open"))) {
        return record(!strcmp(argv[1], "seal"), argv[2], argv[3], argv[4]);
    }
    fprintf(stderr, "usage: spake2 alice|bob <password> | seal|open <key material> <sequence> <data>\n");
    return 2;
}
