/***********************************************************************************************************************************
S3 Storage
***********************************************************************************************************************************/
#include "build.auto.h"

#include <string.h>

#include "common/crypto/hash.h"
#include "common/debug.h"
#include "common/io/http/client.h"
#include "common/io/http/common.h"
#include "common/io/socket/client.h"
#include "common/io/tls/client.h"
#include "common/log.h"
#include "common/regExp.h"
#include "common/type/json.h"
#include "common/type/object.h"
#include "common/type/xml.h"
#include "storage/posix/storage.h"
#include "storage/s3/read.h"
#include "storage/s3/write.h"

/***********************************************************************************************************************************
Defaults
***********************************************************************************************************************************/
#define STORAGE_S3_DELETE_MAX                                       1000

/***********************************************************************************************************************************
S3 HTTP headers
***********************************************************************************************************************************/
STRING_STATIC(S3_HEADER_CONTENT_SHA256_STR,                         "x-amz-content-sha256");
STRING_STATIC(S3_HEADER_DATE_STR,                                   "x-amz-date");
STRING_STATIC(S3_HEADER_TOKEN_STR,                                  "x-amz-security-token");
STRING_STATIC(S3_HEADER_SRVSDENC_STR,                               "x-amz-server-side-encryption");
STRING_STATIC(S3_HEADER_SRVSDENC_KMS_STR,                           "aws:kms");
STRING_STATIC(S3_HEADER_SRVSDENC_KMSKEYID_STR,                      "x-amz-server-side-encryption-aws-kms-key-id");
STRING_STATIC(S3_HEADER_SSECUSTKEY_ALGO_STR,                        "x-amz-server-side-encryption-customer-algorithm");
STRING_STATIC(S3_HEADER_SSECUSTKEY_AES256_STR,                      "AES256");
STRING_STATIC(S3_HEADER_SSECUSTKEY_KEY_STR,                         "x-amz-server-side-encryption-customer-key");
STRING_STATIC(S3_HEADER_SSECUSTKEY_KEY_MD5_STR,                     "x-amz-server-side-encryption-customer-key-md5");
STRING_STATIC(S3_HEADER_TAGGING,                                    "x-amz-tagging");
STRING_STATIC(S3_HEADER_REQUEST_PAYER,                              "x-amz-request-payer");
STRING_STATIC(S3_HEADER_REQUEST_PAYER_STR,                          "requester");

/***********************************************************************************************************************************
S3 query tokens
***********************************************************************************************************************************/
STRING_STATIC(S3_QUERY_CONTINUATION_TOKEN_STR,                      "continuation-token");
STRING_STATIC(S3_QUERY_DELETE_STR,                                  "delete");
STRING_STATIC(S3_QUERY_DELIMITER_STR,                               "delimiter");
STRING_STATIC(S3_QUERY_LIST_TYPE_STR,                               "list-type");
STRING_STATIC(S3_QUERY_PREFIX_STR,                                  "prefix");
STRING_STATIC(S3_QUERY_VERSIONS_STR,                                "versions");

STRING_STATIC(S3_QUERY_VALUE_LIST_TYPE_2_STR,                       "2");

/***********************************************************************************************************************************
XML tags
***********************************************************************************************************************************/
STRING_STATIC(S3_XML_TAG_COMMON_PREFIXES_STR,                       "CommonPrefixes");
STRING_STATIC(S3_XML_TAG_CONTENTS_STR,                              "Contents");
STRING_STATIC(S3_XML_TAG_DELETE_STR,                                "Delete");
STRING_STATIC(S3_XML_TAG_DELETE_MARKER_STR,                         "DeleteMarker");
STRING_STATIC(S3_XML_TAG_ERROR_STR,                                 "Error");
STRING_STATIC(S3_XML_TAG_IS_TRUNCATED_STR,                          "IsTruncated");
STRING_STATIC(S3_XML_TAG_KEY_STR,                                   "Key");
STRING_STATIC(S3_XML_TAG_LAST_MODIFIED_STR,                         "LastModified");
#define S3_XML_TAG_NEXT_CONTINUATION_TOKEN                          "NextContinuationToken"
STRING_STATIC(S3_XML_TAG_NEXT_CONTINUATION_TOKEN_STR,               S3_XML_TAG_NEXT_CONTINUATION_TOKEN);
STRING_STATIC(S3_XML_TAG_OBJECT_STR,                                "Object");
STRING_STATIC(S3_XML_TAG_PREFIX_STR,                                "Prefix");
STRING_STATIC(S3_XML_TAG_QUIET_STR,                                 "Quiet");
STRING_STATIC(S3_XML_TAG_SIZE_STR,                                  "Size");
STRING_STATIC(S3_XML_TAG_VERSION_STR,                               "Version");
STRING_STATIC(S3_XML_TAG_VERSION_ID_STR,                            "VersionId");

/***********************************************************************************************************************************
AWS authentication v4 constants
***********************************************************************************************************************************/
#define S3                                                          "s3"
BUFFER_STRDEF_STATIC(S3_BUF,                                        S3);
#define AWS4                                                        "AWS4"
#define AWS4_REQUEST                                                "aws4_request"
BUFFER_STRDEF_STATIC(AWS4_REQUEST_BUF,                              AWS4_REQUEST);
#define AWS4_HMAC_SHA256                                            "AWS4-HMAC-SHA256"

/***********************************************************************************************************************************
Starting date for signing string so it will be regenerated on the first request
***********************************************************************************************************************************/
STRING_STATIC(YYYYMMDD_STR,                                         "YYYYMMDD");

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
struct StorageS3
{
    STORAGE_COMMON_MEMBER;
    HttpClient *httpClient;                                         // HTTP client to service requests
    StringList *headerRedactList;                                   // List of headers to redact from logging

    const String *bucket;                                           // Bucket to store data in
    const String *region;                                           // e.g. us-east-1
    StorageS3KeyType keyType;                                       // Key type (e.g. storageS3KeyTypeShared)
    String *accessKey;                                              // Access key
    String *secretAccessKey;                                        // Secret access key
    String *securityToken;                                          // Security token, if any
    const String *kmsKeyId;                                         // Server-side encryption key
    const String *sseCustomerKey;                                   // Base64 of SSE-C encryption key
    const String *sseCustomerKeyMd5;                                // Base64 of MD5 of SSE-C key
    size_t partSize;                                                // Part size for multi-part upload
    const String *tag;                                              // Tags to be applied to objects
    unsigned int deleteMax;                                         // Maximum objects that can be deleted in one request
    StorageS3UriStyle uriStyle;                                     // Path or host style URIs
    const String *bucketEndpoint;                                   // Set to {bucket}.{endpoint}
    bool requesterPays;                                             // Requester pays?

    // For retrieving temporary security credentials
    HttpClient *credHttpClient;                                     // HTTP client to service credential requests
    const String *credHost;                                         // Credentials host
    const String *credRole;                                         // Role to use for credential requests
    const String *webIdTokenFile;                                   // File containing token to use for web-id credential requests
    time_t credExpirationTime;                                      // Time the temporary credentials expire

    // Current signing key and date it is valid for
    const String *signingKeyDate;                                   // Date of cached signing key (so we know when to regenerate)
    const Buffer *signingKey;                                       // Cached signing key
};

/***********************************************************************************************************************************
Expected ISO-8601 data/time size
***********************************************************************************************************************************/
#define ISO_8601_DATE_TIME_SIZE                                     16

/***********************************************************************************************************************************
Generate authorization header and add it to the supplied header list

Based on the excellent documentation at http://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html.
***********************************************************************************************************************************/
static void
storageS3Auth(
    StorageS3 *const this, const String *const verb, const String *const path, const HttpQuery *const query,
    const String *const dateTime, HttpHeader *const httpHeader, const String *const payloadHash)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_S3, this);
        FUNCTION_TEST_PARAM(STRING, verb);
        FUNCTION_TEST_PARAM(STRING, path);
        FUNCTION_TEST_PARAM(HTTP_QUERY, query);
        FUNCTION_TEST_PARAM(STRING, dateTime);
        FUNCTION_TEST_PARAM(KEY_VALUE, httpHeader);
        FUNCTION_TEST_PARAM(STRING, payloadHash);
    FUNCTION_TEST_END();

    ASSERT(verb != NULL);
    ASSERT(path != NULL);
    ASSERT(dateTime != NULL);
    ASSERT(httpHeader != NULL);
    ASSERT(payloadHash != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Get date from datetime
        const String *const date = strSubN(dateTime, 0, 8);

        // Set required headers
        httpHeaderPut(httpHeader, S3_HEADER_CONTENT_SHA256_STR, payloadHash);
        httpHeaderPut(httpHeader, S3_HEADER_DATE_STR, dateTime);
        httpHeaderPut(httpHeader, HTTP_HEADER_HOST_STR, this->bucketEndpoint);

        if (this->securityToken != NULL)
            httpHeaderPut(httpHeader, S3_HEADER_TOKEN_STR, this->securityToken);

        // Generate canonical request and signed headers
        const StringList *const headerList = strLstSort(strLstDup(httpHeaderList(httpHeader)), sortOrderAsc);
        String *signedHeaders = NULL;

        String *const canonicalRequest = strCatFmt(
            strNew(), "%s\n%s\n%s\n", strZ(verb), strZ(path), query == NULL ? "" : strZ(httpQueryRenderP(query)));

        for (unsigned int headerIdx = 0; headerIdx < strLstSize(headerList); headerIdx++)
        {
            const String *const headerKey = strLstGet(headerList, headerIdx);
            const String *const headerKeyLower = strLower(strDup(headerKey));

            // Skip the authorization (exists on retry) and content-length headers since they do not need to be signed
            if (strEq(headerKeyLower, HTTP_HEADER_AUTHORIZATION_STR) || strEq(headerKeyLower, HTTP_HEADER_CONTENT_LENGTH_STR))
                continue;

            strCatFmt(canonicalRequest, "%s:%s\n", strZ(headerKeyLower), strZ(httpHeaderGet(httpHeader, headerKey)));

            if (signedHeaders == NULL)
                signedHeaders = strCat(strNew(), headerKeyLower);
            else
                strCatFmt(signedHeaders, ";%s", strZ(headerKeyLower));
        }

        strCatFmt(canonicalRequest, "\n%s\n%s", strZ(signedHeaders), strZ(payloadHash));

        // Generate string to sign
        const String *const stringToSign = strNewFmt(
            AWS4_HMAC_SHA256 "\n%s\n%s/%s/" S3 "/" AWS4_REQUEST "\n%s", strZ(dateTime), strZ(date), strZ(this->region),
            strZ(strNewEncode(encodingHex, cryptoHashOne(hashTypeSha256, BUFSTR(canonicalRequest)))));

        // Generate signing key. This key only needs to be regenerated every seven days but we'll do it once a day to keep the
        // logic simple. It's a relatively expensive operation so we'd rather not do it for every request.
        // If the cached signing key has expired (or has none been generated) then regenerate it
        if (!strEq(date, this->signingKeyDate))
        {
            const Buffer *const dateKey = cryptoHmacOne(
                hashTypeSha256, BUFSTR(strNewFmt(AWS4 "%s", strZ(this->secretAccessKey))), BUFSTR(date));
            const Buffer *const regionKey = cryptoHmacOne(hashTypeSha256, dateKey, BUFSTR(this->region));
            const Buffer *const serviceKey = cryptoHmacOne(hashTypeSha256, regionKey, S3_BUF);

            // Switch to the object context so signing key and date are not lost
            MEM_CONTEXT_OBJ_BEGIN(this)
            {
                this->signingKey = cryptoHmacOne(hashTypeSha256, serviceKey, AWS4_REQUEST_BUF);
                this->signingKeyDate = strDup(date);
            }
            MEM_CONTEXT_OBJ_END();
        }

        // Generate authorization header
        const String *const authorization = strNewFmt(
            AWS4_HMAC_SHA256 " Credential=%s/%s/%s/" S3 "/" AWS4_REQUEST ",SignedHeaders=%s,Signature=%s",
            strZ(this->accessKey), strZ(date), strZ(this->region), strZ(signedHeaders),
            strZ(strNewEncode(encodingHex, cryptoHmacOne(hashTypeSha256, this->signingKey, BUFSTR(stringToSign)))));

        httpHeaderPut(httpHeader, HTTP_HEADER_AUTHORIZATION_STR, authorization);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_TEST_RETURN_VOID();
}

/***********************************************************************************************************************************
Convert YYYY-MM-DDTHH:MM:SS.MSECZ format to time_t. This format is very nearly ISO-8601 except for the inclusion of milliseconds,
which are discarded here.
***********************************************************************************************************************************/
static time_t
storageS3CvtTime(const String *const time)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, time);
    FUNCTION_TEST_END();

    FUNCTION_TEST_RETURN(
        TIME,
        epochFromParts(
            cvtZSubNToInt(strZ(time), 0, 4), cvtZSubNToInt(strZ(time), 5, 2), cvtZSubNToInt(strZ(time), 8, 2),
            cvtZSubNToInt(strZ(time), 11, 2), cvtZSubNToInt(strZ(time), 14, 2), cvtZSubNToInt(strZ(time), 17, 2), 0));
}

/***********************************************************************************************************************************
Automatically get credentials for an associated IAM role

Documentation for the response format is found at: https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/iam-roles-for-amazon-ec2.html
Documentation for IMDSv2 tokens: https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/instancedata-data-retrieval.html
***********************************************************************************************************************************/
STRING_STATIC(S3_CREDENTIAL_HOST_STR,                               "169.254.169.254");
#define S3_CREDENTIAL_PORT                                          80
#define S3_CREDENTIAL_PATH                                          "/latest/meta-data/iam/security-credentials"
#define S3_CREDENTIAL_RENEW_SEC                                     (5 * 60)

VARIANT_STRDEF_STATIC(S3_JSON_TAG_ACCESS_KEY_ID_VAR,                "AccessKeyId");
VARIANT_STRDEF_STATIC(S3_JSON_TAG_CODE_VAR,                         "Code");
VARIANT_STRDEF_STATIC(S3_JSON_TAG_EXPIRATION_VAR,                   "Expiration");
VARIANT_STRDEF_STATIC(S3_JSON_TAG_SECRET_ACCESS_KEY_VAR,            "SecretAccessKey");
VARIANT_STRDEF_STATIC(S3_JSON_TAG_TOKEN_VAR,                        "Token");

VARIANT_STRDEF_STATIC(S3_JSON_VALUE_SUCCESS_VAR,                    "Success");

static void
storageS3AuthAuto(StorageS3 *const this, HttpHeader *const header)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(HTTP_HEADER, header);
    FUNCTION_LOG_END();

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Retrieve the IMDSv2 token. Do not store the token because it will likely be expired by the time the credentials expire
        // since credentials have a much longer default expiration. The TTL needs to be long enough to survive retries of the two
        // metadata queries.
        HttpHeader *const tokenHeader = httpHeaderDup(header, NULL);
        httpHeaderAdd(
            tokenHeader, STRDEF("x-aws-ec2-metadata-token-ttl-seconds"),
            strNewFmt("%" PRIu64, httpClientTimeout(this->credHttpClient) / 1000 * 3));

        HttpRequest *request = httpRequestNewP(
            this->credHttpClient, HTTP_VERB_PUT_STR, STRDEF("/latest/api/token"), .header = tokenHeader);
        HttpResponse *response = httpRequestResponse(request, true);

        // Set IMDSv2 token on success. If the token request fails for any reason assume that IMDSv2 is not supported. If the
        // instance only supports IMDSv2 then subsequent metadata requests will fail because of the missing token.
        if (httpResponseCodeOk(response))
            httpHeaderAdd(header, STRDEF("x-aws-ec2-metadata-token"), strNewBuf(httpResponseContent(response)));

        // If the role was not set explicitly or retrieved previously then retrieve it
        if (this->credRole == NULL)
        {
            // Request the role
            HttpRequest *const request = httpRequestNewP(
                this->credHttpClient, HTTP_VERB_GET_STR, STRDEF(S3_CREDENTIAL_PATH), .header = header);
            HttpResponse *const response = httpRequestResponse(request, true);

            // Not found likely means no role is associated with this instance
            if (httpResponseCode(response) == HTTP_RESPONSE_CODE_NOT_FOUND)
            {
                THROW(
                    ProtocolError,
                    "role to retrieve temporary credentials not found\n"
                    "HINT: is a valid IAM role associated with this instance?");
            }
            // Else an error that we can't handle
            else if (!httpResponseCodeOk(response))
                httpRequestError(request, response);

            // Get role from the text response
            MEM_CONTEXT_OBJ_BEGIN(this)
            {
                this->credRole = strNewBuf(httpResponseContent(response));
            }
            MEM_CONTEXT_OBJ_END();
        }

        // Retrieve the temp credentials
        request = httpRequestNewP(
            this->credHttpClient, HTTP_VERB_GET_STR, strNewFmt(S3_CREDENTIAL_PATH "/%s", strZ(this->credRole)), .header = header);
        response = httpRequestResponse(request, true);

        // Not found likely means the role is not valid
        if (httpResponseCode(response) == HTTP_RESPONSE_CODE_NOT_FOUND)
        {
            THROW_FMT(
                ProtocolError,
                "role '%s' not found\n"
                "HINT: is '%s' a valid IAM role associated with this instance?",
                strZ(this->credRole), strZ(this->credRole));
        }
        // Else an error that we can't handle
        else if (!httpResponseCodeOk(response))
            httpRequestError(request, response);

        // Get credentials from the JSON response
        const KeyValue *const credential = varKv(jsonToVar(strNewBuf(httpResponseContent(response))));

        MEM_CONTEXT_OBJ_BEGIN(this)
        {
            // Check the code field for errors
            const Variant *const code = kvGetDefault(credential, S3_JSON_TAG_CODE_VAR, VARSTRDEF("code field is missing"));
            CHECK(FormatError, code != NULL, "error code missing");

            if (!varEq(code, S3_JSON_VALUE_SUCCESS_VAR))
                THROW_FMT(FormatError, "unable to retrieve temporary credentials: %s", strZ(varStr(code)));

            // Make sure the required values are present
            CHECK(FormatError, kvGet(credential, S3_JSON_TAG_ACCESS_KEY_ID_VAR) != NULL, "access key missing");
            CHECK(FormatError, kvGet(credential, S3_JSON_TAG_SECRET_ACCESS_KEY_VAR) != NULL, "secret access key missing");
            CHECK(FormatError, kvGet(credential, S3_JSON_TAG_TOKEN_VAR) != NULL, "token missing");

            // Copy credentials
            this->accessKey = strDup(varStr(kvGet(credential, S3_JSON_TAG_ACCESS_KEY_ID_VAR)));
            this->secretAccessKey = strDup(varStr(kvGet(credential, S3_JSON_TAG_SECRET_ACCESS_KEY_VAR)));
            this->securityToken = strDup(varStr(kvGet(credential, S3_JSON_TAG_TOKEN_VAR)));
        }
        MEM_CONTEXT_OBJ_END();

        // Update expiration time
        CHECK(FormatError, kvGet(credential, S3_JSON_TAG_EXPIRATION_VAR) != NULL, "expiration missing");
        this->credExpirationTime = storageS3CvtTime(varStr(kvGet(credential, S3_JSON_TAG_EXPIRATION_VAR)));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

/***********************************************************************************************************************************
Automatically get credentials for an associated web identity service account

Documentation is found at: https://docs.aws.amazon.com/STS/latest/APIReference/API_AssumeRoleWithWebIdentity.html
***********************************************************************************************************************************/
STRING_STATIC(S3_STS_HOST_STR,                                      "sts.amazonaws.com");
#define S3_STS_PORT                                                 443

static void
storageS3AuthWebId(StorageS3 *const this, const HttpHeader *const header)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(HTTP_HEADER, header);
    FUNCTION_LOG_END();

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Load the token from the given file for each request since the token may be updated during execution
        const String *const webIdToken = strNewBuf(
            storageGetP(storageNewReadP(storagePosixNewP(FSLASH_STR), this->webIdTokenFile)));

        // Get credentials
        HttpQuery *const query = httpQueryNewP();
        httpQueryAdd(query, STRDEF("Action"), STRDEF("AssumeRoleWithWebIdentity"));
        httpQueryAdd(query, STRDEF("RoleArn"), this->credRole);
        httpQueryAdd(query, STRDEF("RoleSessionName"), STRDEF(PROJECT_NAME));
        httpQueryAdd(query, STRDEF("Version"), STRDEF("2011-06-15"));
        httpQueryAdd(query, STRDEF("WebIdentityToken"), webIdToken);

        HttpRequest *const request = httpRequestNewP(
            this->credHttpClient, HTTP_VERB_GET_STR, FSLASH_STR, .header = header, .query = query);
        HttpResponse *const response = httpRequestResponse(request, true);

        CHECK(FormatError, httpResponseCode(response) != HTTP_RESPONSE_CODE_NOT_FOUND, "invalid response code");

        // Copy credentials
        const XmlNode *const xmlCred =
            xmlNodeChild(
                xmlNodeChild(
                    xmlDocumentRoot(xmlDocumentNewBuf(httpResponseContent(response))), STRDEF("AssumeRoleWithWebIdentityResult"), true),
                STRDEF("Credentials"), true);

        const XmlNode *const accessKeyNode = xmlNodeChild(xmlCred, STRDEF("AccessKeyId"), true);
        const XmlNode *const secretAccessKeyNode = xmlNodeChild(xmlCred, STRDEF("SecretAccessKey"), true);
        const XmlNode *const securityTokenNode = xmlNodeChild(xmlCred, STRDEF("SessionToken"), true);

        MEM_CONTEXT_OBJ_BEGIN(this)
        {
            this->accessKey = xmlNodeContent(accessKeyNode);
            this->secretAccessKey = xmlNodeContent(secretAccessKeyNode);
            this->securityToken = xmlNodeContent(securityTokenNode);
        }
        MEM_CONTEXT_OBJ_END();

        // Update expiration time
        this->credExpirationTime = storageS3CvtTime(xmlNodeContent(xmlNodeChild(xmlCred, STRDEF("Expiration"), true)));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

/***********************************************************************************************************************************
Process S3 request
***********************************************************************************************************************************/
FN_EXTERN HttpRequest *
storageS3RequestAsync(StorageS3 *const this, const String *const verb, const String *path, const StorageS3RequestAsyncParam param)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(STRING, verb);
        FUNCTION_LOG_PARAM(STRING, path);
        FUNCTION_LOG_PARAM(HTTP_HEADER, param.header);
        FUNCTION_LOG_PARAM(HTTP_QUERY, param.query);
        FUNCTION_LOG_PARAM(BUFFER, param.content);
        FUNCTION_LOG_PARAM(BOOL, param.contentMd5);
        FUNCTION_LOG_PARAM(BOOL, param.sseKms);
        FUNCTION_LOG_PARAM(BOOL, param.sseC);
        FUNCTION_LOG_PARAM(BOOL, param.tag);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(verb != NULL);
    ASSERT(path != NULL);

    HttpRequest *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        HttpHeader *const requestHeader =
            param.header == NULL ? httpHeaderNew(this->headerRedactList) : httpHeaderDup(param.header, this->headerRedactList);

        // Set content length
        httpHeaderAdd(
            requestHeader, HTTP_HEADER_CONTENT_LENGTH_STR,
            param.content == NULL || bufEmpty(param.content) ? ZERO_STR : strNewFmt("%zu", bufUsed(param.content)));

        // Calculate content-md5 header when required
        if (param.contentMd5)
        {
            ASSERT(param.content != NULL && !bufEmpty(param.content));

            httpHeaderAdd(
                requestHeader, HTTP_HEADER_CONTENT_MD5_STR,
                strNewEncode(encodingBase64, cryptoHashOne(hashTypeMd5, param.content)));
        }

        // Set requester pays when requested
        if (this->requesterPays)
            httpHeaderPut(requestHeader, S3_HEADER_REQUEST_PAYER, S3_HEADER_REQUEST_PAYER_STR);

        // Set KMS headers when requested
        if (param.sseKms && this->kmsKeyId != NULL)
        {
            httpHeaderPut(requestHeader, S3_HEADER_SRVSDENC_STR, S3_HEADER_SRVSDENC_KMS_STR);
            httpHeaderPut(requestHeader, S3_HEADER_SRVSDENC_KMSKEYID_STR, this->kmsKeyId);
        }

        // Set SSE-C headers when requested
        if (param.sseC && this->sseCustomerKey != NULL)
        {
            httpHeaderPut(requestHeader, S3_HEADER_SSECUSTKEY_ALGO_STR, S3_HEADER_SSECUSTKEY_AES256_STR);
            httpHeaderPut(requestHeader, S3_HEADER_SSECUSTKEY_KEY_STR, this->sseCustomerKey);
            httpHeaderPut(requestHeader, S3_HEADER_SSECUSTKEY_KEY_MD5_STR, this->sseCustomerKeyMd5);
        }

        // Set tags when requested and available
        if (param.tag && this->tag != NULL)
            httpHeaderPut(requestHeader, S3_HEADER_TAGGING, this->tag);

        // When using path-style URIs the bucket name needs to be prepended
        if (this->uriStyle == storageS3UriStylePath)
            path = strNewFmt("/%s%s", strZ(this->bucket), strZ(path));

        // If temp credentials will be expiring soon then renew them
        if (this->credExpirationTime != 0 && (this->credExpirationTime - time(NULL)) < S3_CREDENTIAL_RENEW_SEC)
        {
            // Free old credentials
            strFree(this->accessKey);
            strFree(this->secretAccessKey);
            strFree(this->securityToken);

            // Set content-length and host headers
            HttpHeader *credHeader = httpHeaderNew(NULL);
            httpHeaderAdd(credHeader, HTTP_HEADER_CONTENT_LENGTH_STR, ZERO_STR);
            httpHeaderAdd(credHeader, HTTP_HEADER_HOST_STR, this->credHost);

            // Get credentials
            switch (this->keyType)
            {
                // Auto authentication
                case storageS3KeyTypeAuto:
                    storageS3AuthAuto(this, credHeader);
                    break;

                // Web identity authentication
                default:
                {
                    ASSERT(this->keyType == storageS3KeyTypeWebId);

                    storageS3AuthWebId(this, credHeader);
                    break;
                }
            }

            // Reset the signing key date so the signing key gets regenerated
            this->signingKeyDate = YYYYMMDD_STR;
        }

        // Encode path
        path = httpUriEncode(path, true);

        // Generate authorization header
        storageS3Auth(
            this, verb, path, param.query, strNewTimeP("%Y%m%dT%H%M%SZ", time(NULL), .utc = true), requestHeader,
            strNewEncode(
                encodingHex,
                param.content == NULL || bufEmpty(param.content) ?
                    HASH_TYPE_SHA256_ZERO_BUF : cryptoHashOne(hashTypeSha256, param.content)));

        // Send request
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = httpRequestNewP(
                this->httpClient, verb, path, .query = param.query, .header = requestHeader, .content = param.content);
        }
        MEM_CONTEXT_END();
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(HTTP_REQUEST, result);
}

FN_EXTERN HttpResponse *
storageS3Response(HttpRequest *const request, const StorageS3ResponseParam param)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(HTTP_REQUEST, request);
        FUNCTION_LOG_PARAM(BOOL, param.allowMissing);
        FUNCTION_LOG_PARAM(BOOL, param.contentIo);
    FUNCTION_LOG_END();

    ASSERT(request != NULL);

    HttpResponse *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Get response
        result = httpRequestResponse(request, !param.contentIo);

        // Error if the request was not successful
        if (!httpResponseCodeOk(result) && (!param.allowMissing || httpResponseCode(result) != HTTP_RESPONSE_CODE_NOT_FOUND))
            httpRequestError(request, result);

        // Move response to the prior context
        httpResponseMove(result, memContextPrior());
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(HTTP_RESPONSE, result);
}

FN_EXTERN HttpResponse *
storageS3Request(StorageS3 *const this, const String *const verb, const String *const path, const StorageS3RequestParam param)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(STRING, verb);
        FUNCTION_LOG_PARAM(STRING, path);
        FUNCTION_LOG_PARAM(HTTP_HEADER, param.header);
        FUNCTION_LOG_PARAM(HTTP_QUERY, param.query);
        FUNCTION_LOG_PARAM(BUFFER, param.content);
        FUNCTION_LOG_PARAM(BOOL, param.contentMd5);
        FUNCTION_LOG_PARAM(BOOL, param.allowMissing);
        FUNCTION_LOG_PARAM(BOOL, param.contentIo);
        FUNCTION_LOG_PARAM(BOOL, param.sseKms);
        FUNCTION_LOG_PARAM(BOOL, param.sseC);
        FUNCTION_LOG_PARAM(BOOL, param.tag);
    FUNCTION_LOG_END();

    HttpRequest *const request = storageS3RequestAsyncP(
        this, verb, path, .header = param.header, .query = param.query, .content = param.content, .contentMd5 = param.contentMd5,
        .sseKms = param.sseKms, .sseC = param.sseC, .tag = param.tag);
    HttpResponse *const result = storageS3ResponseP(
        request, .allowMissing = param.allowMissing, .contentIo = param.contentIo);

    httpRequestFree(request);

    FUNCTION_LOG_RETURN(HTTP_RESPONSE, result);
}

/***********************************************************************************************************************************
General function for listing files to be used by other list routines
***********************************************************************************************************************************/
static void
storageS3ListInternal(
    StorageS3 *const this, const String *const path, const StorageInfoLevel level, const String *const expression,
    const bool recurse, const time_t targetTime, StorageListCallback callback, void *const callbackData)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(STRING, path);
        FUNCTION_LOG_PARAM(ENUM, level);
        FUNCTION_LOG_PARAM(STRING, expression);
        FUNCTION_LOG_PARAM(BOOL, recurse);
        FUNCTION_LOG_PARAM(TIME, targetTime);
        FUNCTION_LOG_PARAM(FUNCTIONP, callback);
        FUNCTION_LOG_PARAM_P(VOID, callbackData);
    FUNCTION_LOG_END();

    FUNCTION_AUDIT_CALLBACK();

    ASSERT(this != NULL);
    ASSERT(path != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Build the base prefix by stripping off the initial /
        const String *const basePrefix = strSize(path) == 1 ? EMPTY_STR : strNewFmt("%s/", strZ(strSub(path, 1)));

        // Get the expression prefix when possible to limit initial results
        const String *const expressionPrefix = regExpPrefix(expression);

        // If there is an expression prefix then use it to build the query prefix, otherwise query prefix is base prefix
        const String *queryPrefix;

        if (expressionPrefix == NULL)
            queryPrefix = basePrefix;
        else
        {
            if (strEmpty(basePrefix))
                queryPrefix = expressionPrefix;
            else
                queryPrefix = strNewFmt("%s%s", strZ(basePrefix), strZ(expressionPrefix));
        }

        // Create query
        HttpQuery *const query = httpQueryNewP();

        // Add the delimiter to not recurse
        if (!recurse)
            httpQueryAdd(query, S3_QUERY_DELIMITER_STR, FSLASH_STR);

        // Use list type 2 or versions as specified
        if (targetTime != 0)
            httpQueryAdd(query, S3_QUERY_VERSIONS_STR, EMPTY_STR);
        else
            httpQueryAdd(query, S3_QUERY_LIST_TYPE_STR, S3_QUERY_VALUE_LIST_TYPE_2_STR);

        // Don't specify empty prefix because it is the default
        if (!strEmpty(queryPrefix))
            httpQueryAdd(query, S3_QUERY_PREFIX_STR, queryPrefix);

        // Store last info so it can be updated across requests for versioning
        String *const nameLast = strNew();
        String *const versionIdLast = strNew();
        StorageInfo infoLast = {.level = level, .name = nameLast};

        if (targetTime != 0)
            infoLast.versionId = versionIdLast;

        // Loop as long as a continuation token returned
        HttpRequest *request = NULL;

        do
        {
            // Use an inner mem context here because we could potentially be retrieving millions of files so it is a good idea to
            // free memory at regular intervals
            MEM_CONTEXT_TEMP_BEGIN()
            {
                HttpResponse *response = NULL;

                // If there is an outstanding async request then wait for the response
                if (request != NULL)
                {
                    response = storageS3ResponseP(request);

                    httpRequestFree(request);
                    request = NULL;
                }
                // Else get the response immediately from a sync request
                else
                    response = storageS3RequestP(this, HTTP_VERB_GET_STR, FSLASH_STR, .query = query);

                const XmlNode *const xmlRoot = xmlDocumentRoot(xmlDocumentNewBuf(httpResponseContent(response)));

                // If list is truncated then send an async request to get more data
                if (strEq(xmlNodeContent(xmlNodeChild(xmlRoot, S3_XML_TAG_IS_TRUNCATED_STR, true)), TRUE_STR))
                {
                    const String *const nextContinuationToken = xmlNodeContent(
                        xmlNodeChild(xmlRoot, S3_XML_TAG_NEXT_CONTINUATION_TOKEN_STR, true));
                    CHECK(FormatError, !strEmpty(nextContinuationToken), S3_XML_TAG_NEXT_CONTINUATION_TOKEN " may not be empty");

                    httpQueryPut(query, S3_QUERY_CONTINUATION_TOKEN_STR, nextContinuationToken);

                    // Store request in the outer temp context
                    MEM_CONTEXT_PRIOR_BEGIN()
                    {
                        request = storageS3RequestAsyncP(this, HTTP_VERB_GET_STR, FSLASH_STR, .query = query);
                    }
                    MEM_CONTEXT_PRIOR_END();
                }

                // Get prefix list
                const XmlNodeList *const subPathList = xmlNodeChildList(xmlRoot, S3_XML_TAG_COMMON_PREFIXES_STR);

                for (unsigned int subPathIdx = 0; subPathIdx < xmlNodeLstSize(subPathList); subPathIdx++)
                {
                    const XmlNode *const subPathNode = xmlNodeLstGet(subPathList, subPathIdx);

                    // Get path name
                    StorageInfo info =
                    {
                        .level = level,
                        .name = xmlNodeContent(xmlNodeChild(subPathNode, S3_XML_TAG_PREFIX_STR, true)),
                        .exists = true,
                    };

                    // Strip off base prefix and final /
                    info.name = strSubN(info.name, strSize(basePrefix), strSize(info.name) - strSize(basePrefix) - 1);

                    // Add type info if requested
                    if (level >= storageInfoLevelType)
                        info.type = storageTypePath;

                    // Callback with info
                    callback(callbackData, &info);
                }

                // Get file list
                const XmlNodeList *fileList;

                if (targetTime != 0)
                {
                    StringList *const nameList = strLstNew();
                    strLstAdd(nameList, S3_XML_TAG_VERSION_STR);
                    strLstAdd(nameList, S3_XML_TAG_DELETE_MARKER_STR);

                    fileList = xmlNodeChildListMulti(xmlRoot, nameList);
                }
                else
                    fileList = xmlNodeChildList(xmlRoot, S3_XML_TAG_CONTENTS_STR);

                for (unsigned int fileIdx = 0; fileIdx < xmlNodeLstSize(fileList); fileIdx++)
                {
                    const XmlNode *const fileNode = xmlNodeLstGet(fileList, fileIdx);

                    // Get file name and strip off the base prefix when present
                    const String *name = xmlNodeContent(xmlNodeChild(fileNode, S3_XML_TAG_KEY_STR, true));

                    if (!strEmpty(basePrefix))
                        name = strSub(name, strSize(basePrefix));

                    // Return info for last file if new file
                    if (infoLast.exists && !strEq(name, nameLast))
                    {
                        callback(callbackData, &infoLast);
                        infoLast.exists = false;
                    }

                    // If targeting by time
                    if (targetTime != 0)
                    {
                        // Skip later versions
                        infoLast.timeModified = storageS3CvtTime(
                            xmlNodeContent(xmlNodeChild(fileNode, S3_XML_TAG_LAST_MODIFIED_STR, true)));

                        if (infoLast.timeModified > targetTime)
                            continue;

                        // If a version has already been returned (or delete marker found) then skip this version
                        if (strEq(infoLast.name, name))
                            continue;

                        // If most recent version is a delete marker then the file will not be returned
                        const bool deleteMarker = strEq(xmlNodeName(fileNode), S3_XML_TAG_DELETE_MARKER_STR);

                        if (deleteMarker)
                        {
                            strCat(strTrunc(nameLast), name);
                            infoLast.exists = false;
                            continue;
                        }
                    }

                    // Update last name and set exists
                    strCat(strTrunc(nameLast), name);
                    infoLast.exists = true;

                    // Add basic info if requested (no need to add type info since file is default type)
                    if (level >= storageInfoLevelBasic)
                    {
                        if (targetTime != 0)
                        {
                            strCat(
                                strTrunc(versionIdLast), xmlNodeContent(xmlNodeChild(fileNode, S3_XML_TAG_VERSION_ID_STR, true)));
                        }
                        else
                        {
                            infoLast.timeModified = storageS3CvtTime(
                                xmlNodeContent(xmlNodeChild(fileNode, S3_XML_TAG_LAST_MODIFIED_STR, true)));
                        }

                        infoLast.size = cvtZToUInt64(strZ(xmlNodeContent(xmlNodeChild(fileNode, S3_XML_TAG_SIZE_STR, true))));
                    }
                }
            }
            MEM_CONTEXT_TEMP_END();
        }
        while (request != NULL);

        // Callback with last info if it exists
        if (infoLast.exists)
            callback(callbackData, &infoLast);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
static StorageInfo
storageS3Info(THIS_VOID, const String *const file, const StorageInfoLevel level, const StorageInterfaceInfoParam param)
{
    THIS(StorageS3);

    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(STRING, file);
        FUNCTION_LOG_PARAM(ENUM, level);
        (void)param;                                                // No parameters are used
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(file != NULL);

    // Attempt to get file info
    HttpResponse *const httpResponse = storageS3RequestP(this, HTTP_VERB_HEAD_STR, file, .allowMissing = true, .sseC = true);

    // Does the file exist?
    StorageInfo result = {.level = level, .exists = httpResponseCodeOk(httpResponse)};

    // Add basic level info if requested and the file exists (no need to add type info since file is default type)
    if (result.level >= storageInfoLevelBasic && result.exists)
    {
        const HttpHeader *const httpHeader = httpResponseHeader(httpResponse);

        const String *const contentLength = httpHeaderGet(httpHeader, HTTP_HEADER_CONTENT_LENGTH_STR);
        CHECK(FormatError, contentLength != NULL, "content length missing");
        result.size = cvtZToUInt64(strZ(contentLength));

        const String *const lastModified = httpHeaderGet(httpHeader, HTTP_HEADER_LAST_MODIFIED_STR);
        CHECK(FormatError, lastModified != NULL, "last modified missing");
        result.timeModified = httpDateToTime(lastModified);
    }

    httpResponseFree(httpResponse);

    FUNCTION_LOG_RETURN(STORAGE_INFO, result);
}

/**********************************************************************************************************************************/
static void
storageS3ListCallback(void *const callbackData, const StorageInfo *const info)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM_P(VOID, callbackData);
        FUNCTION_TEST_PARAM(STORAGE_INFO, info);
    FUNCTION_TEST_END();

    ASSERT(callbackData != NULL);
    ASSERT(info != NULL);

    storageLstAdd(callbackData, info);

    FUNCTION_TEST_RETURN_VOID();
}

static StorageList *
storageS3List(THIS_VOID, const String *const path, const StorageInfoLevel level, const StorageInterfaceListParam param)
{
    THIS(StorageS3);

    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(STRING, path);
        FUNCTION_LOG_PARAM(ENUM, level);
        FUNCTION_LOG_PARAM(STRING, param.expression);
        FUNCTION_LOG_PARAM(TIME, param.targetTime);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(path != NULL);

    StorageList *const result = storageLstNew(level);

    storageS3ListInternal(
        this, path, level, param.expression, false, param.targetTime, storageS3ListCallback, result);

    FUNCTION_LOG_RETURN(STORAGE_LIST, result);
}

/**********************************************************************************************************************************/
static StorageRead *
storageS3NewRead(THIS_VOID, const String *const file, const bool ignoreMissing, const StorageInterfaceNewReadParam param)
{
    THIS(StorageS3);

    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(STRING, file);
        FUNCTION_LOG_PARAM(BOOL, ignoreMissing);
        FUNCTION_LOG_PARAM(UINT64, param.offset);
        FUNCTION_LOG_PARAM(VARIANT, param.limit);
        FUNCTION_LOG_PARAM(BOOL, param.version);
        FUNCTION_LOG_PARAM(STRING, param.versionId);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(file != NULL);

    FUNCTION_LOG_RETURN(
        STORAGE_READ, storageReadS3New(this, file, ignoreMissing, param.offset, param.limit, param.version, param.versionId));
}

/**********************************************************************************************************************************/
static StorageWrite *
storageS3NewWrite(THIS_VOID, const String *const file, const StorageInterfaceNewWriteParam param)
{
    THIS(StorageS3);

    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(STRING, file);
        (void)param;                                                // No parameters are used
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(file != NULL);
    ASSERT(param.createPath);
    ASSERT(param.truncate);
    ASSERT(param.user == NULL);
    ASSERT(param.group == NULL);
    ASSERT(param.timeModified == 0);

    FUNCTION_LOG_RETURN(STORAGE_WRITE, storageWriteS3New(this, file, this->partSize));
}

/**********************************************************************************************************************************/
typedef struct StorageS3PathRemoveData
{
    StorageS3 *this;                                                // Storage object
    MemContext *memContext;                                         // Mem context to create xml document in
    unsigned int size;                                              // Size of delete request
    HttpRequest *request;                                           // Async delete request
    XmlDocument *xml;                                               // Delete xml
    const String *path;                                             // Root path of remove
} StorageS3PathRemoveData;

static HttpRequest *
storageS3PathRemoveInternal(StorageS3 *const this, HttpRequest *const request, XmlDocument *const xml)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STORAGE_S3, this);
        FUNCTION_TEST_PARAM(HTTP_REQUEST, request);
        FUNCTION_TEST_PARAM(XML_DOCUMENT, xml);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    // Get response for async request
    if (request != NULL)
    {
        MEM_CONTEXT_TEMP_BEGIN()
        {
            const Buffer *const response = httpResponseContent(storageS3ResponseP(request));

            // Nothing is returned when there are no errors
            if (!bufEmpty(response))
            {
                const XmlNodeList *const errorList = xmlNodeChildList(
                    xmlDocumentRoot(xmlDocumentNewBuf(response)), S3_XML_TAG_ERROR_STR);

                // Attempt to remove errored files one at a time rather than retrying the batch
                for (unsigned int errorIdx = 0; errorIdx < xmlNodeLstSize(errorList); errorIdx++)
                {
                    storageS3RequestP(
                        this, HTTP_VERB_DELETE_STR,
                        strNewFmt(
                            "/%s",
                            strZ(xmlNodeContent(xmlNodeChild(xmlNodeLstGet(errorList, errorIdx), S3_XML_TAG_KEY_STR, true)))));
                }
            }
        }
        MEM_CONTEXT_TEMP_END();

        httpRequestFree(request);
    }

    // Send new async request if there is more to remove
    HttpRequest *result = NULL;

    if (xml != NULL)
    {
        HttpQuery *const query = httpQueryAdd(httpQueryNewP(), S3_QUERY_DELETE_STR, EMPTY_STR);
        Buffer *const content = xmlDocumentBuf(xml);

        result = storageS3RequestAsyncP(
            this, HTTP_VERB_POST_STR, FSLASH_STR, .query = query, .content = content, .contentMd5 = true);

        httpQueryFree(query);
        bufFree(content);
    }

    FUNCTION_TEST_RETURN(HTTP_REQUEST, result);
}

static void
storageS3PathRemoveCallback(void *const callbackData, const StorageInfo *const info)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM_P(VOID, callbackData);
        FUNCTION_TEST_PARAM(STORAGE_INFO, info);
    FUNCTION_TEST_END();

    ASSERT(callbackData != NULL);
    ASSERT(info != NULL);

    // Only delete files since paths don't really exist
    if (info->type == storageTypeFile)
    {
        StorageS3PathRemoveData *const data = (StorageS3PathRemoveData *)callbackData;

        // If there is something to delete then create the request
        if (data->xml == NULL)
        {
            MEM_CONTEXT_BEGIN(data->memContext)
            {
                data->xml = xmlDocumentNew(S3_XML_TAG_DELETE_STR);
                xmlNodeContentSet(xmlNodeAdd(xmlDocumentRoot(data->xml), S3_XML_TAG_QUIET_STR), TRUE_STR);
            }
            MEM_CONTEXT_END();
        }

        // Add to delete list
        MEM_CONTEXT_TEMP_BEGIN()
        {
            xmlNodeContentSet(
                xmlNodeAdd(xmlNodeAdd(xmlDocumentRoot(data->xml), S3_XML_TAG_OBJECT_STR), S3_XML_TAG_KEY_STR),
                strNewFmt("%s%s", strZ(data->path), strZ(info->name)));
        }
        MEM_CONTEXT_TEMP_END();

        data->size++;

        // Delete list when it is full
        if (data->size == data->this->deleteMax)
        {
            MEM_CONTEXT_BEGIN(data->memContext)
            {
                data->request = storageS3PathRemoveInternal(data->this, data->request, data->xml);
            }
            MEM_CONTEXT_END();

            xmlDocumentFree(data->xml);
            data->xml = NULL;
            data->size = 0;
        }
    }

    FUNCTION_TEST_RETURN_VOID();
}

static bool
storageS3PathRemove(THIS_VOID, const String *const path, const bool recurse, const StorageInterfacePathRemoveParam param)
{
    THIS(StorageS3);

    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(STRING, path);
        FUNCTION_LOG_PARAM(BOOL, recurse);
        (void)param;                                                // No parameters are used
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(path != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        StorageS3PathRemoveData data =
        {
            .this = this,
            .memContext = memContextCurrent(),
            .path = strEq(path, FSLASH_STR) ? EMPTY_STR : strNewFmt("%s/", strZ(strSub(path, 1))),
        };

        storageS3ListInternal(this, path, storageInfoLevelType, NULL, true, 0, storageS3PathRemoveCallback, &data);

        // Call if there is more to be removed
        if (data.xml != NULL)
            data.request = storageS3PathRemoveInternal(this, data.request, data.xml);

        // Check response on last async request
        storageS3PathRemoveInternal(this, data.request, NULL);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(BOOL, true);
}

/**********************************************************************************************************************************/
static void
storageS3Remove(THIS_VOID, const String *const file, const StorageInterfaceRemoveParam param)
{
    THIS(StorageS3);

    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE_S3, this);
        FUNCTION_LOG_PARAM(STRING, file);
        FUNCTION_LOG_PARAM(BOOL, param.errorOnMissing);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(file != NULL);
    ASSERT(!param.errorOnMissing);

    httpResponseFree(storageS3RequestP(this, HTTP_VERB_DELETE_STR, file));

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
static const StorageInterface storageInterfaceS3 =
{
    .feature = 1 << storageFeatureVersioning,

    .info = storageS3Info,
    .list = storageS3List,
    .newRead = storageS3NewRead,
    .newWrite = storageS3NewWrite,
    .pathRemove = storageS3PathRemove,
    .remove = storageS3Remove,
};

FN_EXTERN Storage *
storageS3New(
    const String *const path, const bool write, const time_t targetTime, StoragePathExpressionCallback pathExpressionFunction,
    const String *const bucket, const String *const endPoint, const StorageS3UriStyle uriStyle, const String *const region,
    const StorageS3KeyType keyType, const String *const accessKey, const String *const secretAccessKey,
    const String *const securityToken, const String *const kmsKeyId, const String *sseCustomerKey, const String *const credRole,
    const String *const webIdTokenFile, const size_t partSize, const KeyValue *const tag, const String *host,
    const unsigned int port, const TimeMSec timeout, const bool verifyPeer, const String *const caFile, const String *const caPath,
    const bool requesterPays)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STRING, path);
        FUNCTION_LOG_PARAM(BOOL, write);
        FUNCTION_LOG_PARAM(TIME, targetTime);
        FUNCTION_LOG_PARAM(FUNCTIONP, pathExpressionFunction);
        FUNCTION_LOG_PARAM(STRING, bucket);
        FUNCTION_LOG_PARAM(STRING, endPoint);
        FUNCTION_LOG_PARAM(STRING_ID, uriStyle);
        FUNCTION_LOG_PARAM(STRING, region);
        FUNCTION_LOG_PARAM(STRING_ID, keyType);
        FUNCTION_TEST_PARAM(STRING, accessKey);
        FUNCTION_TEST_PARAM(STRING, secretAccessKey);
        FUNCTION_TEST_PARAM(STRING, securityToken);
        FUNCTION_TEST_PARAM(STRING, kmsKeyId);
        FUNCTION_TEST_PARAM(STRING, sseCustomerKey);
        FUNCTION_TEST_PARAM(STRING, credRole);
        FUNCTION_TEST_PARAM(STRING, webIdTokenFile);
        FUNCTION_LOG_PARAM(SIZE, partSize);
        FUNCTION_LOG_PARAM(KEY_VALUE, tag);
        FUNCTION_LOG_PARAM(STRING, host);
        FUNCTION_LOG_PARAM(UINT, port);
        FUNCTION_LOG_PARAM(TIME_MSEC, timeout);
        FUNCTION_LOG_PARAM(BOOL, verifyPeer);
        FUNCTION_LOG_PARAM(STRING, caFile);
        FUNCTION_LOG_PARAM(STRING, caPath);
    FUNCTION_LOG_END();

    ASSERT(path != NULL);
    ASSERT(bucket != NULL);
    ASSERT(endPoint != NULL);
    ASSERT(region != NULL);
    ASSERT(partSize != 0);

    OBJ_NEW_BEGIN(StorageS3, .childQty = MEM_CONTEXT_QTY_MAX)
    {
        *this = (StorageS3)
        {
            .interface = storageInterfaceS3,
            .bucket = strDup(bucket),
            .region = strDup(region),
            .keyType = keyType,
            .kmsKeyId = strDup(kmsKeyId),
            .requesterPays = requesterPays,
            .sseCustomerKey = strDup(sseCustomerKey),
            .partSize = partSize,
            .deleteMax = STORAGE_S3_DELETE_MAX,
            .uriStyle = uriStyle,
            .bucketEndpoint =
                uriStyle == storageS3UriStyleHost ? strNewFmt("%s.%s", strZ(bucket), strZ(endPoint)) : strDup(endPoint),

            // Force the signing key to be generated on the first run
            .signingKeyDate = YYYYMMDD_STR,
        };

        // Create tag query string
        if (write && tag != NULL)
        {
            HttpQuery *const query = httpQueryNewP(.kv = tag);
            this->tag = httpQueryRenderP(query);
            httpQueryFree(query);
        }

        // Create the HTTP client used to service requests
        if (host == NULL)
            host = this->bucketEndpoint;

        this->httpClient = httpClientNew(
            tlsClientNewP(
                sckClientNew(host, port, timeout, timeout), host, timeout, timeout, verifyPeer, .caFile = caFile, .caPath = caPath),
            timeout);

        // Initialize authentication
        switch (this->keyType)
        {
            // Create the HTTP client used to retrieve temporary security credentials
            case storageS3KeyTypeAuto:
            {
                ASSERT(accessKey == NULL && secretAccessKey == NULL && securityToken == NULL);

                this->credRole = strDup(credRole);
                this->credHost = S3_CREDENTIAL_HOST_STR;
                this->credExpirationTime = time(NULL);
                this->credHttpClient = httpClientNew(
                    sckClientNew(this->credHost, S3_CREDENTIAL_PORT, timeout, timeout), timeout);

                break;
            }

            // Create the HTTP client used to retrieve web identity security credentials
            case storageS3KeyTypeWebId:
            {
                ASSERT(accessKey == NULL && secretAccessKey == NULL && securityToken == NULL);
                ASSERT(credRole != NULL);
                ASSERT(webIdTokenFile != NULL);

                this->credRole = strDup(credRole);
                this->webIdTokenFile = strDup(webIdTokenFile);
                this->credHost = S3_STS_HOST_STR;
                this->credExpirationTime = time(NULL);
                this->credHttpClient = httpClientNew(
                    tlsClientNewP(
                        sckClientNew(this->credHost, S3_STS_PORT, timeout, timeout), this->credHost, timeout, timeout, true,
                        .caFile = caFile, .caPath = caPath),
                    timeout);

                break;
            }

            // Set shared key credentials
            default:
            {
                ASSERT(this->keyType == storageS3KeyTypeShared);
                ASSERT(accessKey != NULL && secretAccessKey != NULL);

                this->accessKey = strDup(accessKey);
                this->secretAccessKey = strDup(secretAccessKey);
                this->securityToken = strDup(securityToken);

                break;
            }
        }

        // Generate SSE customer key MD5 hash
        if (this->sseCustomerKey != NULL)
        {
            this->sseCustomerKeyMd5 = strNewEncode(
                encodingBase64, cryptoHashOne(hashTypeMd5, bufNewDecode(encodingBase64, this->sseCustomerKey)));
        }

        // Create list of redacted headers
        this->headerRedactList = strLstNew();
        strLstAdd(this->headerRedactList, HTTP_HEADER_AUTHORIZATION_STR);
        strLstAdd(this->headerRedactList, S3_HEADER_DATE_STR);
        strLstAdd(this->headerRedactList, S3_HEADER_SSECUSTKEY_KEY_STR);
        strLstAdd(this->headerRedactList, S3_HEADER_SSECUSTKEY_KEY_MD5_STR);
        strLstAdd(this->headerRedactList, S3_HEADER_TOKEN_STR);
    }
    OBJ_NEW_END();

    FUNCTION_LOG_RETURN(
        STORAGE, storageNew(STORAGE_S3_TYPE, path, 0, 0, write, targetTime, pathExpressionFunction, this, this->interface));
}
