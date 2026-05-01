/* ---------------------------------------------------------------------------------------
 * 
 * license.cpp
 * 
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * 
 * IDENTIFICATION
 *        src/common/backend/utils/misc/license.cpp
 *
 * ---------------------------------------------------------------------------------------
 */
#include <string.h>
#include <time.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include "utils/license.h"



const char MAGIC[] = "LIC2025";
const size_t BUFFER_SIZE = 4096;
#define MD5_LENGTH 2 * MD5_DIGEST_LENGTH
unsigned char* cipher_text = nullptr;
size_t cipher_text_len = 0;

const char PUBKEY[] = "-----BEGIN PUBLIC KEY-----\n"
"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAtttfMIx+DIUHayBet1WJ\n"
"am0kv/UUVpjlirsohsQDfnNL5FAmD2xMdJtIh0drVmZeeRSUWLT1sm7HU5BD+sFe\n"
"fkb/80Q6prLV4J4hU8s6fETkK6VACg4ls9XIriazpPPdXL5qjyh3q39dFSbASKXB\n"
"a1cb5kLrcx9p7Betyapa+K0L08+qj5ue08aZna/Wzc2zxj9Q+iLlDK8Z9K334TUD\n"
"SNcLEhtkRE01T/N98xdWyL9aqYHcxnLEfYdyT7j8ZXd/eEVz8Kz4WNdQAMCFz7+8\n"
"bdwNnRh+nYFH4oWqv7Qyv+zZTgLCuK1jg28wkvj72TILEz5YYt4zpzXp0yTkuccv\n"
"8wIDAQAB\n"
"-----END PUBLIC KEY-----";
const char PRIKEY[] = "-----BEGIN PRIVATE KEY-----\n"
"MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC2218wjH4MhQdr\n"
"IF63VYlqbSS/9RRWmOWKuyiGxAN+c0vkUCYPbEx0m0iHR2tWZl55FJRYtPWybsdT\n"
"kEP6wV5+Rv/zRDqmstXgniFTyzp8ROQrpUAKDiWz1ciuJrOk891cvmqPKHerf10V\n"
"JsBIpcFrVxvmQutzH2nsF63Jqlr4rQvTz6qPm57Txpmdr9bNzbPGP1D6IuUMrxn0\n"
"rffhNQNI1wsSG2RETTVP833zF1bIv1qpgdzGcsR9h3JPuPxld394RXPwrPhY11AA\n"
"wIXPv7xt3A2dGH6dgUfihaq/tDK/7NlOAsK4rWODbzCS+PvZMgsTPlhi3jOnNenT\n"
"JOS5xy/zAgMBAAECggEBAJ0Jew97qIjh+kQDEbTLTe9LeoMsW+Ie/wsMvro2PnXr\n"
"WKLkPjuCm4qNDVW4fTM/SSUdCmXASz2JK7/VJryEMr2qBggKcYBWF54Gz8Jhx4GP\n"
"vJDLI1s/WRu/ntAJRsCD+ni6w1LcwyFSiUMv+3SofZrMvZYbpI9CzDnJACQwHF5w\n"
"8pjLKUrlASZq5PR0R5mHdYHJoXfThnG9F+9zD1tUi1cLdHDbb/oqGa2bf/a9Thq9\n"
"YiYSiGvL3+br0DOKG/Mxfq5+wstRuL17DM5VXFKAUEky0Fln4jq0WqYT0BfP6Q6L\n"
"w2tkXBeiC0D/pKCBYKnFBEfj8oaSUVsxAbz349pyRNECgYEA48juXXzJaI21HER/\n"
"XkCfIVzzoGAPyRlr+YnxXq2hUr0zTajDRWu8EbNF8ME0EWJQmWnjNJnmRKQUyIkm\n"
"Z14e08MP48xsMVzs7SoJLccIsgpUzLsWS3eTd0UEH5UfaZ8uxxtwjOzfU71Fgnue\n"
"GHo4DKVvzja0G4UPyQjPayxBs6sCgYEAzYHF3ngCTW/fc1Xvppr3zdAW/+EOw6HG\n"
"QCh2HLMhHH7a2LuKKJ4qIsNGdDWNdyaaJJpFi9AfhSUMglf3eTWD/ZyYeOA/Nlxg\n"
"FuHukEReloYjESQyS/PjjnqbuBj5Ylg7+gftClOhQWNnJ2lCQfEJn0DRLjSHnE8P\n"
"AQ5VtSUdrNkCgYEAtdVEUuS2cvwMQ5B6jGbRoPRuluuYLlRY2U7Am2/HhCD8v1Wk\n"
"69ngu9B8WIAibG4rIQxiDy97nffNj3fMbF+6BBmGqbYZ+B3SFFCmGyDzVAzjPLts\n"
"RLojweYaMIv+E3a7BL5mzliYvmQtBMhhn/CQpae65MbOZ9mEjFq4GTmvCRkCgYAK\n"
"mUrPMnlL7R0lIaV+fjeRkc3d3ImaZVmILY4J5OMsSQ6YZvO1LJMXv+J+U9S79G22\n"
"vY4gq9c0UrjWcBr/UVdBWTYz5bzc3N7Hz1cycZQ/RyO/2pINgMKXspMdZ4xVGh/d\n"
"wVLkWXPTn4DOc4tLQ1cvs3QWYfcshJdNgqPA9+0T4QKBgCTcEVxLs7ZV8T77k4Ml\n"
"nJ1L1pPlRAzxqL6JaMLskdA+CDEhHICo3B+FZnUZV5aUikb7xANljFU+/9wnHUdh\n"
"5fIFqhuOHJroeXFzaxCIY+76Q1pJYsIPjfPc+XChJttHRbTj+sonH9acaI//AiHz\n"
"LbWV/1zLZ35lLJQ+M2PwQV/w\n"
"-----END PRIVATE KEY-----";

time_t getCurrentTime()
{
    return time(NULL);
}

void computeMD5(const char* data, size_t data_len, char* md5_str)
{
    unsigned char md5[MD5_DIGEST_LENGTH];
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, data, data_len);
    MD5_Final(md5, &ctx);

    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(md5_str + i*2, "%02x", md5[i]);
    }
}

time_t stringToTime(const char* s)
{
    char* end;
    long result = strtol(s, &end, 10);
    return *end ? -1 : result;
}

typedef struct {
    char* buffer;
    size_t writePos;
    size_t readPos;
    size_t capacity;
} CircularBuffer;

void CircularBuffer_Init(CircularBuffer* cb, size_t size)
{
    cb->buffer = (char*)malloc(size);
    cb->capacity = size;
    cb->writePos = 0;
    cb->readPos = 0;
}

void CircularBuffer_Write(CircularBuffer* cb, const char* data, size_t len)
{
    while (len > 0) {
        size_t space = cb->capacity - cb->writePos;
        size_t copyLen = (len < space) ? len : space;
        memcpy(cb->buffer + cb->writePos, data, copyLen);
        cb->writePos = (cb->writePos + copyLen) % cb->capacity;
        data += copyLen;
        len -= copyLen;
    }
}

void CircularBuffer_Read(CircularBuffer* cb, char* data, size_t len)
{
    while (len > 0) {
        size_t available = cb->capacity - cb->readPos;
        size_t copyLen = (len < available) ? len : available;
        memcpy(data, cb->buffer + cb->readPos, copyLen);
        cb->readPos = (cb->readPos + copyLen) % cb->capacity;
        data += copyLen;
        len -= copyLen;
    }
}

void CircularBuffer_Reset(CircularBuffer* cb)
{
    cb->writePos = 0;
    cb->readPos = 0;
}

void CircularBuffer_Destroy(CircularBuffer* cb)
{
    free(cb->buffer);
}

RSA* loadPublicKey(const char* pemString)
{
    BIO* bio = BIO_new_mem_buf(pemString, -1);
    if (!bio) return NULL;

    RSA* rsa = PEM_read_bio_RSA_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return rsa;
}

RSA* loadPrivateKey(const char* pemString)
{
    BIO* bio = BIO_new_mem_buf(pemString, -1);
    if (!bio) return NULL;

    RSA* rsa = PEM_read_bio_RSAPrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return rsa;
}

int encryptRSA(const char* plaintext, size_t plaintext_len, RSA* rsa, unsigned char** ciphertext, size_t* ciphertext_len)
{
    int rsaLength = RSA_size(rsa);
    if (rsaLength <= 0) {
        printf("Failed to get RSA size\n");
        return 1;
    }

    CircularBuffer cb;
    CircularBuffer_Init(&cb, rsaLength * 2);
    CircularBuffer_Write(&cb, plaintext, plaintext_len);

    *ciphertext = (unsigned char*)malloc(plaintext_len * rsaLength);
    *ciphertext_len = 0;

    while (cb.writePos != cb.readPos) {
        unsigned char chunk[rsaLength];
        int chunkSize = (rsaLength < (int)(cb.writePos - cb.readPos)) ? rsaLength : (int)(cb.writePos - cb.readPos);
        CircularBuffer_Read(&cb, (char*)chunk, chunkSize);

        unsigned char encrypted[rsaLength];
        int decryptedLen = RSA_public_encrypt(chunkSize, chunk, encrypted, rsa, RSA_PKCS1_PADDING);
        if (decryptedLen < 0) {
            printf("RSA encryption failed: %s\n", ERR_error_string(ERR_get_error(), NULL));
            CircularBuffer_Destroy(&cb);
            free(*ciphertext);
            return 1;
        }

        memcpy(*ciphertext + *ciphertext_len, encrypted, decryptedLen);
        *ciphertext_len += decryptedLen;
    }

    CircularBuffer_Destroy(&cb);
    return 0;
}

int decryptRSA(const unsigned char* ciphertext, size_t ciphertext_len, RSA* rsa, char** plaintext, size_t* plaintext_len, char* errmsg)
{
    int rsaLength = RSA_size(rsa);
    if (rsaLength <= 0) {
        sprintf(errmsg, "failed to get RSA size");
        return 1;
    }

    *plaintext = (char*)malloc(ciphertext_len);
    *plaintext_len = 0;

    for (size_t i = 0; i < ciphertext_len; i += rsaLength) {
        unsigned char block[rsaLength + 256];
        int decryptedLen = RSA_private_decrypt(rsaLength, &ciphertext[i], block, rsa, RSA_PKCS1_PADDING);

        if (decryptedLen < 0) {
            sprintf(errmsg, "RSA decryption failed: %s", ERR_error_string(ERR_get_error(), NULL));
            free(*plaintext);
            return 1;
        }

        memcpy(*plaintext + *plaintext_len, block, decryptedLen);
        *plaintext_len += decryptedLen;
    }

    return 0;
}

int validateLicenseContent(const char* plaintext, char* errmsg)
{
    if (strncmp(plaintext, MAGIC, sizeof(MAGIC) - 1) != 0) {
        sprintf(errmsg, "invalid license magic");
        return 1;
    }

    size_t pos = sizeof(MAGIC) - 1;
    const size_t md5Size = 32;

    if (strlen(plaintext) < pos + md5Size) {
        sprintf(errmsg, "license too short");
        return 1;
    }

    char textMd5[md5Size + 1];
    strncpy(textMd5, plaintext + pos, md5Size);
    textMd5[md5Size] = '\0';
    pos += md5Size;

    const u_int8_t textLen = (u_int8_t)plaintext[pos];
    pos += 1;

    if (strlen(plaintext) < pos + textLen) {
        sprintf(errmsg, "license text too short");
        return 1;
    }

    char text[textLen + 1];
    strncpy(text, plaintext + pos, textLen);
    text[textLen] = '\0';

    char computedMd5[MD5_LENGTH + 1];
    computeMD5(text, textLen, computedMd5);
    if (strcmp(textMd5, computedMd5) != 0) {
        sprintf(errmsg, "md5 checksum mismatch");
        return 1;
    }

    const u_int8_t half_textLen =  textLen / 2;
    char from[half_textLen + 1];
    char to[half_textLen + 1];
    strncpy(from, text, half_textLen);
    from[half_textLen] = '\0';
    strncpy(to, text + half_textLen, half_textLen);
    to[half_textLen] = '\0';

    time_t currentTime = getCurrentTime();
    time_t fromTime = stringToTime(from);
    time_t toTime = stringToTime(to);

    if (currentTime < fromTime || currentTime > toTime) {
        sprintf(errmsg, "license expired");
        return 1;
    }

    return 0;
}

int generateLicense(char* outputLicenseFile, time_t from, time_t to)
{
    RSA* rsa = loadPublicKey(PUBKEY);
    if (!rsa) {
        printf("Failed to load public key\n");
        return 1;
    }

    char text[256];
    snprintf(text, sizeof(text), "%ld%ld", from, to);
    char textMd5[MD5_LENGTH + 1];
    computeMD5(text, strlen(text), textMd5);

    char plaintext[1024];
    snprintf(plaintext, sizeof(plaintext), "%s%s", MAGIC, textMd5);
    char * ptr = plaintext + strlen(MAGIC) + strlen(textMd5);
    *ptr = (char)(u_int8_t)strlen(text);
    int offset = strlen(MAGIC) + strlen(textMd5) + 1;
    snprintf(plaintext + offset, sizeof(plaintext) - offset, "%s", text);

    unsigned char* ciphertext;
    size_t ciphertext_len;
    if (encryptRSA(plaintext, strlen(plaintext), rsa, &ciphertext, &ciphertext_len)) {
        RSA_free(rsa);
        return 1;
    }

    FILE* licenseFile = fopen(outputLicenseFile, "wb");
    if (!licenseFile) {
        RSA_free(rsa);
        free(ciphertext);
        return 1;
    }

    fwrite(ciphertext, 1, ciphertext_len, licenseFile);
    fclose(licenseFile);
    RSA_free(rsa);
    free(ciphertext);

    return 0;
}

int verifyLicense(const char* licenseFile, char* errmsg) 
{
    RSA* rsa = loadPrivateKey(PRIKEY);
    if (!rsa) {
        sprintf(errmsg, "failed to load private key");
        return 1;
    }

    if (cipher_text == nullptr) {
        FILE* file = fopen(licenseFile, "rb");
        if (!file) {
            RSA_free(rsa);
            sprintf(errmsg, "failed to open license file");
            return 1;
        }

        fseek(file, 0, SEEK_END);
        size_t fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);

        cipher_text = (unsigned char*)malloc(fileSize);
        fread(cipher_text, 1, fileSize, file);
        fclose(file);
        cipher_text_len = fileSize;
    }
    

    char* plaintext;
    size_t plaintext_len;
    if (decryptRSA(cipher_text, cipher_text_len, rsa, &plaintext, &plaintext_len, errmsg)) {
        RSA_free(rsa);
        return 1;
    }

    RSA_free(rsa);

    int valid = validateLicenseContent(plaintext, errmsg);
    free(plaintext);

    return valid;
}