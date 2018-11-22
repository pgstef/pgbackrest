/***********************************************************************************************************************************
Http Query
***********************************************************************************************************************************/
#include "common/debug.h"
#include "common/io/http/common.h"
#include "common/io/http/query.h"
#include "common/memContext.h"
#include "common/type/keyValue.h"

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
struct HttpQuery
{
    MemContext *memContext;                                         // Mem context
    KeyValue *kv;                                                   // KeyValue store
};

/***********************************************************************************************************************************
New object
***********************************************************************************************************************************/
HttpQuery *
httpQueryNew(void)
{
    FUNCTION_TEST_VOID();

    HttpQuery *this = NULL;

    MEM_CONTEXT_NEW_BEGIN("HttpQuery")
    {
        // Allocate state and set context
        this = memNew(sizeof(HttpQuery));
        this->memContext = MEM_CONTEXT_NEW();
        this->kv = kvNew();
    }
    MEM_CONTEXT_NEW_END();

    FUNCTION_TEST_RESULT(HTTP_QUERY, this);
}

/***********************************************************************************************************************************
Add a query item
***********************************************************************************************************************************/
HttpQuery *
httpQueryAdd(HttpQuery *this, const String *key, const String *value)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(HTTP_QUERY, this);
        FUNCTION_TEST_PARAM(STRING, key);
        FUNCTION_TEST_PARAM(STRING, value);

        FUNCTION_TEST_ASSERT(this != NULL);
        FUNCTION_TEST_ASSERT(key != NULL);
        FUNCTION_TEST_ASSERT(value != NULL);
    FUNCTION_TEST_END();

    MEM_CONTEXT_BEGIN(this->memContext)
    {
        // Make sure the key does not already exist
        Variant *keyVar = varNewStr(key);

        if (kvGet(this->kv, keyVar) != NULL)
            THROW_FMT(AssertError, "key '%s' already exists", strPtr(key));

        // Store the key
        kvPut(this->kv, keyVar, varNewStr(value));
    }
    MEM_CONTEXT_END();

    FUNCTION_TEST_RESULT(HTTP_QUERY, this);
}

/***********************************************************************************************************************************
Get a value using the key
***********************************************************************************************************************************/
const String *
httpQueryGet(const HttpQuery *this, const String *key)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(HTTP_QUERY, this);
        FUNCTION_TEST_PARAM(STRING, key);

        FUNCTION_TEST_ASSERT(this != NULL);
        FUNCTION_TEST_ASSERT(key != NULL);
    FUNCTION_TEST_END();

    String *result = NULL;

    MEM_CONTEXT_BEGIN(this->memContext)
    {
        result = varStr(kvGet(this->kv, varNewStr(key)));
    }
    MEM_CONTEXT_END();

    FUNCTION_TEST_RESULT(STRING, result);
}

/***********************************************************************************************************************************
Get list of keys
***********************************************************************************************************************************/
StringList *
httpQueryList(const HttpQuery *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(HTTP_QUERY, this);

        FUNCTION_TEST_ASSERT(this != NULL);
    FUNCTION_TEST_END();

    FUNCTION_TEST_RESULT(STRING_LIST, strLstSort(strLstNewVarLst(kvKeyList(this->kv)), sortOrderAsc));
}

/***********************************************************************************************************************************
Move object to a new mem context
***********************************************************************************************************************************/
HttpQuery *
httpQueryMove(HttpQuery *this, MemContext *parentNew)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(HTTP_QUERY, this);
        FUNCTION_TEST_PARAM(MEM_CONTEXT, parentNew);

        FUNCTION_TEST_ASSERT(parentNew != NULL);
    FUNCTION_TEST_END();

    if (this != NULL)
        memContextMove(this->memContext, parentNew);

    FUNCTION_TEST_RESULT(HTTP_QUERY, this);
}

/***********************************************************************************************************************************
Put a query item
***********************************************************************************************************************************/
HttpQuery *
httpQueryPut(HttpQuery *this, const String *key, const String *value)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(HTTP_QUERY, this);
        FUNCTION_TEST_PARAM(STRING, key);
        FUNCTION_TEST_PARAM(STRING, value);

        FUNCTION_TEST_ASSERT(this != NULL);
        FUNCTION_TEST_ASSERT(key != NULL);
        FUNCTION_TEST_ASSERT(value != NULL);
    FUNCTION_TEST_END();

    MEM_CONTEXT_BEGIN(this->memContext)
    {
        // Store the key
        kvPut(this->kv, varNewStr(key), varNewStr(value));
    }
    MEM_CONTEXT_END();

    FUNCTION_TEST_RESULT(HTTP_QUERY, this);
}

/***********************************************************************************************************************************
Render the query for inclusion in an http request
***********************************************************************************************************************************/
String *
httpQueryRender(const HttpQuery *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(HTTP_QUERY, this);
    FUNCTION_TEST_END();

    String *result = NULL;

    if (this != NULL)
    {
        const StringList *keyList = httpQueryList(this);

        if (strLstSize(keyList) > 0)
        {
            result = strNew("");

            MEM_CONTEXT_TEMP_BEGIN()
            {
                for (unsigned int keyIdx = 0; keyIdx < strLstSize(keyList); keyIdx++)
                {
                    if (strSize(result) != 0)
                        strCat(result, "&");

                    strCatFmt(
                        result, "%s=%s", strPtr(strLstGet(keyList, keyIdx)),
                        strPtr(httpUriEncode(httpQueryGet(this, strLstGet(keyList, keyIdx)), false)));
                }
            }
            MEM_CONTEXT_TEMP_END();
        }
    }

    FUNCTION_TEST_RESULT(STRING, result);
}

/***********************************************************************************************************************************
Render as string for logging
***********************************************************************************************************************************/
String *
httpQueryToLog(const HttpQuery *this)
{
    String *result = strNew("{");
    const StringList *keyList = httpQueryList(this);

    for (unsigned int keyIdx = 0; keyIdx < strLstSize(keyList); keyIdx++)
    {
        if (strSize(result) != 1)
            strCat(result, ", ");

        strCatFmt(
            result, "%s: '%s'", strPtr(strLstGet(keyList, keyIdx)),
            strPtr(httpQueryGet(this, strLstGet(keyList, keyIdx))));
    }

    strCat(result, "}");

    return result;
}

/***********************************************************************************************************************************
Free the object
***********************************************************************************************************************************/
void
httpQueryFree(HttpQuery *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(HTTP_QUERY, this);
    FUNCTION_TEST_END();

    if (this != NULL)
        memContextFree(this->memContext);

    FUNCTION_TEST_RESULT_VOID();
}
