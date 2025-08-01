/*
 * corePKCS11 v3.6.3
 * Copyright (C) 2024 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file core_pkcs11_mbedtls.c
 * @brief mbedTLS-based PKCS#11 implementation for software keys. This
 * file deviates from the FreeRTOS style standard for some function names and
 * data types in order to maintain compliance with the PKCS #11 standard.
 */

/* PKCS #11 includes. */
#include "core_pkcs11_config.h"
#include "core_pkcs11_config_defaults.h"
#include "core_pkcs11.h"
#include "core_pkcs11_pal.h"
#include "core_pki_utils.h"
#include "pkcs11t.h"

/**
 *  @brief Declaring MBEDTLS_ALLOW_PRIVATE_ACCESS allows access to mbedtls "private" fields.
 */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/* MbedTLS includes. */
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/sha256.h"
#include "mbedtls/cmac.h"
#include "mbedtls/platform.h"
#include "mbedtls/threading.h"
#include "mbedtls/error.h"

#ifdef MBEDTLS_PSA_CRYPTO_C
    #include "psa/crypto.h"
    #include "psa/crypto_values.h"
#endif /* MBEDTLS_PSA_CRYPTO_C */

/* C runtime includes. */
#include <string.h>

#if defined( GENERATED_PRIVATE_KEY_WRITE_PATH )
    #warning "GENERATED_PRIVATE_KEY_WRITE_PATH was defined. C_GenerateKeyPair will write generated private keys to that filepath"
    #include <errno.h>
    #define PRIV_KEY_BUFFER_LENGTH    2048
#endif /* defined( GENERATED_PRIVATE_KEY_WRITE_PATH ) */

/*-----------------------------------------------------------*/

/**
 * @defgroup pkcs11_macros PKCS #11 Implementation Macros
 * @brief Macros for PKCS #11 software implementation.
 */

/**
 * @defgroup pkcs11_datatypes PKCS #11 Datatypes
 * @brief Internal datatypes for PKCS #11 software implementation.
 */

#ifndef DISABLE_LOGGING

/**
 * @brief Represents string to be logged when mbedTLS returned error
 * does not contain a high-level code.
 */
    static const char * pNoHighLevelMbedTlsCodeStr = "<No-High-Level-Code>";

/**
 * @brief Represents string to be logged when mbedTLS returned error
 * does not contain a low-level code.
 */
    static const char * pNoLowLevelMbedTlsCodeStr = "<No-Low-Level-Code>";

/**
 * @brief Utility for converting the high-level code in an mbedTLS error to string,
 * if the code-contains a high-level code; otherwise, using a default string.
 */
    #define mbedtlsHighLevelCodeOrDefault( mbedTlsCode )   \
    ( mbedtls_high_level_strerr( mbedTlsCode ) != NULL ) ? \
    mbedtls_high_level_strerr( mbedTlsCode ) : pNoHighLevelMbedTlsCodeStr

/**
 * @brief Utility for converting the level-level code in an mbedTLS error to string,
 * if the code-contains a level-level code; otherwise, using a default string.
 */
    #define mbedtlsLowLevelCodeOrDefault( mbedTlsCode )   \
    ( mbedtls_low_level_strerr( mbedTlsCode ) != NULL ) ? \
    mbedtls_low_level_strerr( mbedTlsCode ) : pNoLowLevelMbedTlsCodeStr

#endif /* ifndef DISABLE_LOGGING */

/**
 * @ingroup pkcs11_macros
 * @brief Delay to wait on acquiring a mutex, in ms.
 */
#define pkcs11MUTEX_WAIT_MS                     ( pdMS_TO_TICKS( 5000U ) )

/**
 * @ingroup pkcs11_macros
 * @brief Indicates that no PKCS #11 operation is underway for given session.
 */
#define pkcs11NO_OPERATION                      ( ( CK_MECHANISM_TYPE ) 0xFFFFFFFFUL )

/**
 * @ingroup pkcs11_macros
 * @brief size of a prime256v1 EC private key in bytes, when encoded in DER.
 */
#define pkcs11_PRIVATE_EC_PRIME_256_DER_SIZE    160

/**
 * @ingroup pkcs11_macros
 * @brief size of a prime256v1 EC public key in bytes, when encoded in DER.
 */
#define pkcs11_PUBLIC_EC_PRIME_256_DER_SIZE     100

/**
 * @ingroup pkcs11_macros
 * @brief size of a 2048 bit RSA public key in bytes, when encoded in DER.
 */
#define pkcs11_PUBLIC_RSA_2048_DER_SIZE         300

/**
 * @ingroup pkcs11_macros
 * @brief size of a 2048 bit RSA private key in bytes, in DER encoding.
 */
#define pkcs11_PRIVATE_RSA_2048_DER_SIZE        1200

/**
 * @ingroup pkcs11_macros
 * @brief Max size of an EC public key in bytes, in DER encoding.
 */
#define pkcs11_MAX_EC_PUBLIC_KEY_DER_SIZE       pkcs11_PUBLIC_EC_PRIME_256_DER_SIZE


/**
 * @ingroup pkcs11_macros
 * @brief Max size of an EC private key in bytes, in DER encoding.
 */
#define pkcs11_MAX_EC_PRIVATE_KEY_DER_SIZE    pkcs11_PRIVATE_EC_PRIME_256_DER_SIZE

/**
 * @ingroup pkcs11_macros
 * @brief Length of bytes to contain an EC point.
 *
 * This port currently only uses prime256v1, in which the fields are 32 bytes in
 * length. The public EC point is as long as the curve's fields * 2 + 1.
 * so the EC point for this port is (32 * 2) + 1 bytes in length.
 *
 * mbed TLS encodes the length of the point in the first byte of the buffer it
 * receives, so an additional 1 byte in length is added to account for this.
 *
 * In addition to this, an additional 1 byte is added to store information
 * indicating that the point is uncompressed.
 *
 * @note This length needs to be updated if using a different curve.
 *
 * To summarize:
 * 32 points of 2 bytes each + 1 point length byte, 1 length byte, and
 * 1 type (uncompressed) byte
 */
#define pkcs11EC_POINT_LENGTH                 ( ( 32UL * 2UL ) + 1UL + 1UL + 1UL )

/**
 * @ingroup pkcs11_macros
 * @brief Max size of a public key.
 * This macro defines the size of a key in bytes, in DER encoding.
 *
 * @note The largest RSA public key is used because EC keys are smaller.
 */
#define pkcs11_MAX_PUBLIC_KEY_DER_SIZE        pkcs11_PUBLIC_RSA_2048_DER_SIZE


/**
 * @ingroup pkcs11_macros
 * @brief Max key length of a key.
 * This macro defines the size of a key in bytes, in DER format.
 *
 * Currently the largest key type supported by this port is a 2048 bit
 * RSA private key.
 *
 * @note The largest RSA private key is used because EC keys are smaller and
 * the RSA public key is smaller.
 */
#define pkcs11_MAX_PRIVATE_KEY_DER_SIZE    pkcs11_PRIVATE_RSA_2048_DER_SIZE

/**
 * @ingroup pkcs11_macros
 * @brief The size of the buffer malloc'ed for the exported public key in C_GenerateKeyPair.
 */
#define pkcs11KEY_GEN_MAX_DER_SIZE         200

/**
 * @ingroup pkcs11_macros
 * @brief The slot ID to be returned by this PKCS #11 implementation.
 *
 * @note that this implementation does not have a concept of "slots" so this number is arbitrary.
 */
#define pkcs11SLOT_ID                      1

/**
 * @ingroup pkcs11_macros
 * @brief Private defines for checking that attribute templates are complete.
 */
#define LABEL_IN_TEMPLATE                  ( 1U )      /**< Bit set for label in template. */
#define PRIVATE_IN_TEMPLATE                ( 1U << 1 ) /**< Bit set for private key in in template. */
#define SIGN_IN_TEMPLATE                   ( 1U << 2 ) /**< Bit set for sign in template. */
#define EC_PARAMS_IN_TEMPLATE              ( 1U << 3 ) /**< Bit set for EC params in template. */
#define VERIFY_IN_TEMPLATE                 ( 1U << 4 ) /**< Bit set for verify in template. */

/**
 * @ingroup pkcs11_macros
 * @brief Macro to signify an invalid PKCS #11 key type.
 */
#define PKCS11_INVALID_KEY_TYPE            ( ( CK_KEY_TYPE ) 0xFFFFFFFFUL )

/**
 * @ingroup pkcs11_macros
 * @brief Private define for minimum SHA256-HMAC key size.
 */
#define PKCS11_SHA256_HMAC_MIN_SIZE        ( 32UL )

/**
 * @ingroup pkcs11_macros
 * @brief Private define for minimum AES-CMAC key size, in bytes.
 */
#define PKCS11_AES_CMAC_MIN_SIZE           ( 16UL )

/**
 * @ingroup pkcs11_macros
 * @brief Private define to inform mbedtls MD module to use an HMAC for the MD context.
 */
#define PKCS11_USING_HMAC                  ( 1 )

/**
 * @brief Define the MBEDTLS_VERSION_NUMBER as 0 if it is not defined, done for MISRA rule 20.9
 */
#ifndef MBEDTLS_VERSION_NUMBER
    #define MBEDTLS_VERSION_NUMBER    0
#endif

/**
 * @ingroup pkcs11_datatypes
 * @brief PKCS #11 object container.
 *
 * Maps a PKCS #11 object handle to it's label.
 */
typedef struct P11Object_t
{
    CK_OBJECT_HANDLE xHandle;                           /**< @brief The "PAL Handle". */
    CK_ULONG xLabelSize;                                /**< @brief Size of label. */
    CK_BYTE xLabel[ pkcs11configMAX_LABEL_LENGTH + 1 ]; /**< @brief Plus 1 for the null terminator. */
} P11Object_t;

/**
 * @ingroup pkcs11_datatypes
 * @brief PKCS #11 object container list.
 *
 * This structure helps the core_pkcs11_mbedtls.c maintain a mapping of all objects in one place.
 * Because some objects exist in device NVM and must be called by their "PAL Handles", and other
 * objects do not have designated NVM storage locations, the ObjectList maintains a list
 * of what object handles are available.
 */
typedef struct P11ObjectList_t
{
    mbedtls_threading_mutex_t xMutex;                    /**< @brief Mutex that protects write operations to the xObjects array. */
    P11Object_t xObjects[ pkcs11configMAX_NUM_OBJECTS ]; /**< @brief List of PKCS #11 objects. */
} P11ObjectList_t;

/**
 * @ingroup pkcs11_datatypes
 * @brief PKCS #11 Module Object
 */
typedef struct P11Struct_t
{
    CK_BBOOL xIsInitialized;                     /**< @brief Indicates whether PKCS #11 module has been initialized with a call to C_Initialize. */
    mbedtls_ctr_drbg_context xMbedDrbgCtx;       /**< @brief CTR-DRBG context for PKCS #11 module - used to generate pseudo-random numbers. */
    mbedtls_entropy_context xMbedEntropyContext; /**< @brief Entropy context for PKCS #11 module - used to collect entropy for RNG. */
    mbedtls_threading_mutex_t xSessionMutex;     /**< @brief Mutex that protects write operations to the pxSession array. */
    P11ObjectList_t xObjectList;                 /**< @brief List of PKCS #11 objects that have been found/created since module initialization.
                                                  *         The array position indicates the "App Handle"  */
} P11Struct_t;


/**
 * @ingroup pkcs11_datatypes
 * @brief Session structure.
 */
typedef struct P11Session
{
    CK_ULONG ulState;                            /**< @brief Stores the session flags. */
    CK_BBOOL xOpened;                            /**< @brief Set to CK_TRUE upon opening PKCS #11 session. */
    CK_MECHANISM_TYPE xOperationDigestMechanism; /**< @brief Indicates if a digest operation is in progress. */
    CK_BYTE * pxFindObjectLabel;                 /**< @brief Pointer to the label for the search in progress. Should be NULL if no search in progress. */
    CK_ULONG xFindObjectLabelLen;                /**< @brief Size of current search label. */
    CK_MECHANISM_TYPE xOperationVerifyMechanism; /**< @brief The mechanism of verify operation in progress. Set during C_VerifyInit. */
    mbedtls_threading_mutex_t xVerifyMutex;      /**< @brief Protects the verification key from being modified while in use. */
    CK_OBJECT_HANDLE xVerifyKeyHandle;           /**< @brief Object handle to the verification key. */
    mbedtls_pk_context xVerifyKey;               /**< @brief Verification key.  Set during C_VerifyInit. */
    CK_MECHANISM_TYPE xOperationSignMechanism;   /**< @brief Mechanism of the sign operation in progress. Set during C_SignInit. */
    mbedtls_threading_mutex_t xSignMutex;        /**< @brief Protects the signing key from being modified while in use. */
    CK_OBJECT_HANDLE xSignKeyHandle;             /**< @brief Object handle to the signing key. */
    mbedtls_pk_context xSignKey;                 /**< @brief Signing key.  Set during C_SignInit. */
    mbedtls_sha256_context xSHA256Context;       /**< @brief Context for in progress digest operation. */
    CK_OBJECT_HANDLE xHMACKeyHandle;             /**< @brief Object handle to the HMAC key. */
    mbedtls_md_context_t xHMACSecretContext;     /**< @brief Context for in progress HMAC operation. Set during C_SignInit or C_VerifyInit. */
    CK_OBJECT_HANDLE xCMACKeyHandle;             /**< @brief Object handle to the CMAC key. */
    mbedtls_cipher_context_t xCMACSecretContext; /**< @brief Context for in progress CMAC operation. Set during C_SignInit or C_VerifyInit. */
} P11Session_t;

/*-----------------------------------------------------------*/

/**
 * @brief The global PKCS #11 module object.
 * Entropy/randomness and object lists are shared across PKCS #11 sessions.
 */
static P11Struct_t xP11Context;

/**
 * @brief The global PKCS #11 session list.
 */
static P11Session_t pxP11Sessions[ pkcs11configMAX_SESSIONS ] = { 0 };

/**
 * @brief Helper to check if the current session is initialized and valid.
 */
static CK_RV prvCheckValidSessionAndModule( const P11Session_t * pxSession )
{
    CK_RV xResult = CKR_OK;

    /** MISRA Rule 10.5 - Cannot cast from unsigned to signed.
     * The rule 10.5 is violated because type of the boolean macros defined by PKCS #11
     * are 0 and 1, which results in a signed integer, meanwhile the underlying
     * type of CK_BBOOL is an unsigned char.
     *
     * This means that our implementation conforms to the exception provided by MISRA
     * To quote MISRA: "An integer constant expression with the value 0 or 1 of either signedness
     * may be cast to a type which is defined as essentially Boolean.
     * This allows the implementation of non-C99 Boolean models."
     */
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    if( xP11Context.xIsInitialized == ( CK_BBOOL ) CK_FALSE )
    {
        LogDebug( ( "Could not get a valid session. PKCS #11 was not initialized since xP11Context.xIsInitialized was CK_FALSE." ) );
        xResult = CKR_CRYPTOKI_NOT_INITIALIZED;
    }
    else if( pxSession == NULL )
    {
        LogDebug( ( "Could not get a valid session. PKCS #11 session handle pSession was NULL." ) );
        xResult = CKR_SESSION_HANDLE_INVALID;
    }
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    else if( pxSession->xOpened == ( CK_BBOOL ) CK_FALSE )
    {
        LogDebug( ( "Could not get a valid session. PKCS #11 session handle was not initialized with a previous call to C_OpenSession." ) );
        xResult = CKR_SESSION_HANDLE_INVALID;
    }
    else
    {
        /* Session is initialized and valid. */
    }

    return xResult;
}

/**
 * @brief Maps an opaque caller session handle into its internal state structure.
 */
static P11Session_t * prvSessionPointerFromHandle( CK_SESSION_HANDLE xSession )
{
    P11Session_t * pxSession = NULL;

    if( ( xSession >= 1UL ) && ( xSession <= pkcs11configMAX_SESSIONS ) )
    {
        /* Decrement by 1, invalid handles in PKCS #11 are defined to be 0. */
        pxSession = &( pxP11Sessions[ xSession - 1UL ] );
    }
    else
    {
        LogDebug( ( "Could not convert from CK_SESSION_HANDLE to P11Session_t pointer. Session handle was out of the valid range. Session handle was: %lu.", ( unsigned long int ) xSession ) );
    }

    return pxSession;
}

/**
 * @brief Determines if an operation is in progress.
 */
static CK_BBOOL prvOperationActive( const P11Session_t * pxSession )
{
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xResult = ( CK_BBOOL ) CK_FALSE;

    if( ( pxSession->xOperationDigestMechanism < pkcs11NO_OPERATION ) == CK_TRUE )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        xResult = ( CK_BBOOL ) CK_TRUE;
    }
    else if( ( pxSession->xOperationSignMechanism < pkcs11NO_OPERATION ) == CK_TRUE )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        xResult = ( CK_BBOOL ) CK_TRUE;
    }
    else if( ( pxSession->xOperationVerifyMechanism < pkcs11NO_OPERATION ) == CK_TRUE )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        xResult = ( CK_BBOOL ) CK_TRUE;
    }
    else if( pxSession->pxFindObjectLabel != NULL )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        xResult = ( CK_BBOOL ) CK_TRUE;
    }
    else
    {
        /* MISRA */
    }

    return xResult;
}

/**
 * @brief Initialize mbedTLS.
 */
static CK_RV prvMbedTLS_Initialize( void )
{
    CK_RV xResult = CKR_OK;

    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    int32_t lMbedTLSResult = 0;

    ( void ) memset( &xP11Context, 0, sizeof( xP11Context ) );

    mbedtls_mutex_init( &xP11Context.xObjectList.xMutex );
    mbedtls_mutex_init( &xP11Context.xSessionMutex );

    /* Initialize the entropy source and DRBG for the PKCS#11 module */
    mbedtls_entropy_init( &xP11Context.xMbedEntropyContext );
    mbedtls_ctr_drbg_init( &xP11Context.xMbedDrbgCtx );

    /* Initialise the global mutexes for the PSA API's */
    #if defined( MBEDTLS_PSA_CRYPTO_C ) && ( MBEDTLS_VERSION_NUMBER >= 0x03060300 )
        mbedtls_mutex_init( &mbedtls_threading_key_slot_mutex );
        mbedtls_mutex_init( &mbedtls_threading_psa_globaldata_mutex );
        mbedtls_mutex_init( &mbedtls_threading_psa_rngdata_mutex );
    #endif

    lMbedTLSResult = mbedtls_ctr_drbg_seed( &xP11Context.xMbedDrbgCtx,
                                            mbedtls_entropy_func,
                                            &xP11Context.xMbedEntropyContext,
                                            NULL,
                                            0 );

    if( 0 != lMbedTLSResult )
    {
        LogError( ( "Could not initialize PKCS #11. Failed to seed the DRBG: mbed TLS error = %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                    mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
        xResult = CKR_FUNCTION_FAILED;
    }
    else
    {
        #ifdef MBEDTLS_PSA_CRYPTO_C
            lMbedTLSResult = psa_crypto_init();

            if( lMbedTLSResult != PSA_SUCCESS )
            {
                LogError( ( "Could not initialize PKCS #11. Failed to initialize PSA: MBedTLS error = %s : %s.",
                            mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                            mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                xResult = CKR_FUNCTION_FAILED;
                /* MISRA Ref 10.5.1 [Essential type casting] */
                /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                /* coverity[misra_c_2012_rule_10_5_violation] */
                xP11Context.xIsInitialized = ( CK_BBOOL ) CK_FALSE;
            }
            else
            {
                LogDebug( ( "MbedTLS PSA module was successfully initialized." ) );
            }
        #endif /* MBEDTLS_PSA_CRYPTO_C */

        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        xP11Context.xIsInitialized = ( CK_BBOOL ) CK_TRUE;
        LogDebug( ( "PKCS #11 module was successfully initialized." ) );
    }

    return xResult;
}

/**
 * @brief Searches a template for the CKA_CLASS attribute.
 */
static CK_RV prvGetObjectClass( const CK_ATTRIBUTE * pxTemplate,
                                CK_ULONG ulCount,
                                CK_OBJECT_CLASS * pxClass )
{
    CK_RV xResult = CKR_TEMPLATE_INCOMPLETE;
    CK_ULONG ulIndex = 0;

    /* Search template for class attribute. */
    for( ulIndex = 0; ulIndex < ulCount; ulIndex++ )
    {
        if( ( pxTemplate[ ulIndex ].type == CKA_CLASS ) &&
            ( pxTemplate[ ulIndex ].ulValueLen == sizeof( CK_OBJECT_CLASS ) ) )
        {
            LogDebug( ( "Successfully found object class attribute." ) );
            ( void ) memcpy( pxClass, pxTemplate[ ulIndex ].pValue,
                             sizeof( CK_OBJECT_CLASS ) );
            xResult = CKR_OK;
            break;
        }
    }

    return xResult;
}

/**
 * @brief Parses attribute values for a certificate.
 */
static CK_RV prvCertAttParse( CK_ATTRIBUTE * pxAttribute,
                              CK_CERTIFICATE_TYPE * pxCertificateType,
                              CK_BYTE_PTR * ppxCertificateValue,
                              CK_ULONG * pxCertificateLength,
                              CK_ATTRIBUTE ** ppxLabel )
{
    CK_RV xResult = CKR_OK;
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xBool = ( CK_BBOOL ) CK_FALSE;

    switch( pxAttribute->type )
    {
        case ( CKA_VALUE ):
            *ppxCertificateValue = pxAttribute->pValue;
            *pxCertificateLength = pxAttribute->ulValueLen;
            break;

        case ( CKA_LABEL ):

            if( pxAttribute->ulValueLen <= pkcs11configMAX_LABEL_LENGTH )
            {
                *ppxLabel = pxAttribute;
            }
            else
            {
                LogError( ( "Failed parsing certificate template. Label length "
                            "was not in the valid range. Found %lu and expected %lu. "
                            "Consider updating pkcs11configMAX_LABEL_LENGTH.",
                            ( unsigned long int ) pxAttribute->ulValueLen,
                            ( unsigned long int ) pkcs11configMAX_LABEL_LENGTH ) );
                xResult = CKR_DATA_LEN_RANGE;
            }

            break;

        case ( CKA_CERTIFICATE_TYPE ):

            if( pxAttribute->ulValueLen == sizeof( CK_CERTIFICATE_TYPE ) )
            {
                ( void ) memcpy( pxCertificateType, pxAttribute->pValue, sizeof( CK_CERTIFICATE_TYPE ) );
            }

            if( *pxCertificateType != CKC_X_509 )
            {
                LogError( ( "Failed parsing certificate template. Certificate type was invalid. "
                            "Expected CKC_X_509, but found 0x%0lX.", ( unsigned long int ) *pxCertificateType ) );
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            }

            break;

        case ( CKA_TOKEN ):

            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, sizeof( CK_BBOOL ) );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool != ( CK_BBOOL ) CK_TRUE )
            {
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            }

            break;

        case ( CKA_CLASS ):
        case ( CKA_SUBJECT ):
            /* Do nothing.  This was already parsed out of the template previously. */
            break;

        default:
            LogError( ( "Failed parsing certificate template. Received an unknown "
                        "template type with value 0x%0lX.", ( unsigned long int ) pxAttribute->type ) );
            xResult = CKR_ATTRIBUTE_TYPE_INVALID;
            break;
    }

    return xResult;
}

/**
 * @brief Parses attribute values for an RSA public Key.
 */
static CK_RV prvRsaPrivKeyAttParse( const CK_ATTRIBUTE * pxAttribute )
{
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xBool = ( CK_BBOOL ) CK_FALSE;
    CK_RV xResult = CKR_OK;

    if( pxAttribute->type == CKA_SIGN )
    {
        if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
        {
            ( void ) memcpy( &xBool, pxAttribute->pValue, pxAttribute->ulValueLen );
        }

        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        if( xBool == ( CK_BBOOL ) CK_FALSE )
        {
            xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            LogError( ( "Failed to parse RSA private key. Expected sign permissions to be supported." ) );
        }
    }

    return xResult;
}

/**
 * @brief Parses attribute values for an RSA public Key.
 */
static CK_RV prvRsaPubKeyAttParse( const CK_ATTRIBUTE * pxAttribute )
{
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xBool = ( CK_BBOOL ) CK_FALSE;
    CK_RV xResult = CKR_OK;

    if( pxAttribute->type == CKA_VERIFY )
    {
        if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
        {
            ( void ) memcpy( &xBool, pxAttribute->pValue, pxAttribute->ulValueLen );
        }

        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        if( xBool == ( CK_BBOOL ) CK_FALSE )
        {
            xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            LogError( ( "Failed to parse RSA public key. Expected verify permissions to be supported." ) );
        }
    }

    return xResult;
}


/**
 * @brief Parses attribute values for an RSA key an puts them in the mbed TLS context.
 */
static CK_RV prvRsaContextParse( const CK_ATTRIBUTE * pxAttribute,
                                 mbedtls_rsa_context * pxRsaContext )
{
    CK_RV xResult = CKR_OK;
    int32_t lMbedTLSResult = 0;

    switch( pxAttribute->type )
    {
        case ( CKA_MODULUS ):
            lMbedTLSResult = mbedtls_rsa_import_raw( pxRsaContext,
                                                     pxAttribute->pValue, pxAttribute->ulValueLen, /* N */
                                                     NULL, 0,                                      /* P */
                                                     NULL, 0,                                      /* Q */
                                                     NULL, 0,                                      /* D */
                                                     NULL, 0 );                                    /* E */
            break;

        case ( CKA_PUBLIC_EXPONENT ):
            lMbedTLSResult = mbedtls_rsa_import_raw( pxRsaContext,
                                                     NULL, 0,                                        /* N */
                                                     NULL, 0,                                        /* P */
                                                     NULL, 0,                                        /* Q */
                                                     NULL, 0,                                        /* D */
                                                     pxAttribute->pValue, pxAttribute->ulValueLen ); /* E */
            break;

        case ( CKA_PRIME_1 ):
            lMbedTLSResult = mbedtls_rsa_import_raw( pxRsaContext,
                                                     NULL, 0,                                      /* N */
                                                     pxAttribute->pValue, pxAttribute->ulValueLen, /* P */
                                                     NULL, 0,                                      /* Q */
                                                     NULL, 0,                                      /* D */
                                                     NULL, 0 );                                    /* E */
            break;

        case ( CKA_PRIME_2 ):
            lMbedTLSResult = mbedtls_rsa_import_raw( pxRsaContext,
                                                     NULL, 0,                                      /* N */
                                                     NULL, 0,                                      /* P */
                                                     pxAttribute->pValue, pxAttribute->ulValueLen, /* Q */
                                                     NULL, 0,                                      /* D */
                                                     NULL, 0 );                                    /* E */
            break;

        case ( CKA_PRIVATE_EXPONENT ):
            lMbedTLSResult = mbedtls_rsa_import_raw( pxRsaContext,
                                                     NULL, 0,                                      /* N */
                                                     NULL, 0,                                      /* P */
                                                     NULL, 0,                                      /* Q */
                                                     pxAttribute->pValue, pxAttribute->ulValueLen, /* D */
                                                     NULL, 0 );                                    /* E */
            break;

        case ( CKA_EXPONENT_1 ):
            lMbedTLSResult = mbedtls_mpi_read_binary( &pxRsaContext->DP, pxAttribute->pValue, pxAttribute->ulValueLen );
            break;

        case ( CKA_EXPONENT_2 ):
            lMbedTLSResult = mbedtls_mpi_read_binary( &pxRsaContext->DQ, pxAttribute->pValue, pxAttribute->ulValueLen );
            break;

        default:
            /* This is the CKA_COEFFICIENT case. The type is checked in prvRsaKeyAttParse. */
            lMbedTLSResult = mbedtls_mpi_read_binary( &pxRsaContext->QP, pxAttribute->pValue, pxAttribute->ulValueLen );
            break;
    }

    if( lMbedTLSResult != 0 )
    {
        LogError( ( "Failed to parse RSA private key template: mbed TLS error = %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                    mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
        xResult = CKR_FUNCTION_FAILED;
    }

    return xResult;
}

/**
 * @brief Populates attribute values for an RSA key from the mbed TLS context.
 */
static CK_RV prvGetAttributesFromRsaContext( CK_ATTRIBUTE * pxAttribute,
                                             const mbedtls_rsa_context * pxRsaContext )
{
    CK_RV xResult = CKR_OK;
    int32_t lMbedTLSResult = 0;
    mbedtls_mpi * pxMpi = ( mbedtls_mpi * ) pxAttribute->pValue;

    mbedtls_mpi_init( pxMpi );

    switch( pxAttribute->type )
    {
        case ( CKA_MODULUS ):

            lMbedTLSResult = mbedtls_mpi_grow( pxMpi, pxRsaContext->N.n );

            if( lMbedTLSResult == 0 )
            {
                lMbedTLSResult = mbedtls_rsa_export( pxRsaContext,
                                                     pxMpi,  /* N */
                                                     NULL,   /* P */
                                                     NULL,   /* Q */
                                                     NULL,   /* D */
                                                     NULL ); /* E */
            }

            break;

        case ( CKA_PUBLIC_EXPONENT ):

            lMbedTLSResult = mbedtls_mpi_grow( pxMpi, pxRsaContext->E.n );

            if( lMbedTLSResult == 0 )
            {
                lMbedTLSResult = mbedtls_rsa_export( pxRsaContext,
                                                     NULL,    /* N */
                                                     NULL,    /* P */
                                                     NULL,    /* Q */
                                                     NULL,    /* D */
                                                     pxMpi ); /* E */
            }

            break;

        case ( CKA_PRIME_1 ):

            lMbedTLSResult = mbedtls_mpi_grow( pxMpi, pxRsaContext->P.n );

            if( lMbedTLSResult == 0 )
            {
                lMbedTLSResult = mbedtls_rsa_export( pxRsaContext,
                                                     NULL,   /* N */
                                                     pxMpi,  /* P */
                                                     NULL,   /* Q */
                                                     NULL,   /* D */
                                                     NULL ); /* E */
            }

            break;

        case ( CKA_PRIME_2 ):

            lMbedTLSResult = mbedtls_mpi_grow( pxMpi, pxRsaContext->Q.n );

            if( lMbedTLSResult == 0 )
            {
                lMbedTLSResult = mbedtls_rsa_export( pxRsaContext,
                                                     NULL,   /* N */
                                                     NULL,   /* P */
                                                     pxMpi,  /* Q */
                                                     NULL,   /* D */
                                                     NULL ); /* E */
            }

            break;

        case ( CKA_PRIVATE_EXPONENT ):

            lMbedTLSResult = mbedtls_mpi_grow( pxMpi, pxRsaContext->D.n );

            if( lMbedTLSResult == 0 )
            {
                lMbedTLSResult = mbedtls_rsa_export( pxRsaContext,
                                                     NULL,   /* N */
                                                     NULL,   /* P */
                                                     NULL,   /* Q */
                                                     pxMpi,  /* D */
                                                     NULL ); /* E */
            }

            break;

        case ( CKA_EXPONENT_1 ):

            lMbedTLSResult = mbedtls_mpi_grow( pxMpi, pxRsaContext->DP.n );

            if( lMbedTLSResult == 0 )
            {
                lMbedTLSResult = mbedtls_rsa_export_crt( pxRsaContext,
                                                         pxMpi,  /* DP */
                                                         NULL,   /* DQ */
                                                         NULL ); /* QP */
            }

            break;

        case ( CKA_EXPONENT_2 ):

            lMbedTLSResult = mbedtls_mpi_grow( pxMpi, pxRsaContext->DQ.n );

            if( lMbedTLSResult == 0 )
            {
                lMbedTLSResult = mbedtls_rsa_export_crt( pxRsaContext,
                                                         NULL,   /* DP */
                                                         pxMpi,  /* DQ */
                                                         NULL ); /* QP */
            }

            break;

        default:

            /* This is the CKA_COEFFICIENT case. The type is checked in
             * C_GetAttributeValue. */
            lMbedTLSResult = mbedtls_mpi_grow( pxMpi, pxRsaContext->QP.n );

            if( lMbedTLSResult == 0 )
            {
                lMbedTLSResult = mbedtls_rsa_export_crt( pxRsaContext,
                                                         NULL,    /* DP */
                                                         NULL,    /* DQ */
                                                         pxMpi ); /* QP */
            }

            break;
    }

    if( lMbedTLSResult != 0 )
    {
        LogError( ( "Failed to parse RSA private key attributes: mbed TLS error = %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                    mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
        xResult = CKR_FUNCTION_FAILED;
    }

    return xResult;
}

/**
 * @brief Parses attribute values for a RSA Key.
 */
static CK_RV prvRsaKeyAttParse( const CK_ATTRIBUTE * pxAttribute,
                                mbedtls_rsa_context * pxRsaContext,
                                CK_BBOOL xIsPrivate )
{
    CK_RV xResult = CKR_OK;
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xBool = ( CK_BBOOL ) CK_FALSE;

    switch( pxAttribute->type )
    {
        case ( CKA_CLASS ):
        case ( CKA_KEY_TYPE ):
        case ( CKA_LABEL ):
            /* Do nothing. These values were parsed previously. */
            break;

        case ( CKA_TOKEN ):

            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, sizeof( CK_BBOOL ) );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool != ( CK_BBOOL ) CK_TRUE )
            {
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            }

            break;

        case ( CKA_VERIFY ):

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xIsPrivate == ( CK_BBOOL ) CK_FALSE )
            {
                xResult = prvRsaPubKeyAttParse( pxAttribute );
            }
            else
            {
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            }

            break;

        case ( CKA_SIGN ):

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xIsPrivate == ( CK_BBOOL ) CK_TRUE )
            {
                xResult = prvRsaPrivKeyAttParse( pxAttribute );
            }
            else
            {
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            }

            break;

        case ( CKA_MODULUS ):
        case ( CKA_PUBLIC_EXPONENT ):
        case ( CKA_PRIME_1 ):
        case ( CKA_PRIME_2 ):
        case ( CKA_PRIVATE_EXPONENT ):
        case ( CKA_EXPONENT_1 ):
        case ( CKA_EXPONENT_2 ):
        case ( CKA_COEFFICIENT ):
            xResult = prvRsaContextParse( pxAttribute, pxRsaContext );
            break;

        default:
            xResult = CKR_ATTRIBUTE_TYPE_INVALID;
            break;
    }

    return xResult;
}

/**
 * @brief Parses attribute values for a private EC Key.
 */
#ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM
    static CK_RV prvEcPrivKeyAttParse( const CK_ATTRIBUTE * pxAttribute,
                                       const mbedtls_pk_context * pxMbedContext )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        CK_BBOOL xBool = ( CK_BBOOL ) CK_FALSE;
        int32_t lMbedTLSResult = 0;
        CK_RV xResult = CKR_OK;

        mbedtls_ecp_keypair * pxKeyPair = ( mbedtls_ecp_keypair * ) pxMbedContext->pk_ctx;

        if( pxAttribute->type == CKA_SIGN )
        {
            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, sizeof( CK_BBOOL ) );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool == ( CK_BBOOL ) CK_FALSE )
            {
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
                LogError( ( "Failed to parse EC private key. Only private keys with signing privileges are supported." ) );
            }
        }
        else
        {
            lMbedTLSResult = mbedtls_mpi_read_binary( &pxKeyPair->d,
                                                      pxAttribute->pValue,
                                                      pxAttribute->ulValueLen );

            if( lMbedTLSResult != 0 )
            {
                xResult = CKR_FUNCTION_FAILED;
                LogError( ( "Failed to parse EC private key. MPI read binary failed: mbed TLS error = %s : %s.",
                            mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                            mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            }
        }

        return xResult;
    }
#endif /* ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM */

/**
 * @brief Parses attribute values for a public EC Key.
 */
#ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM
    static CK_RV prvEcPubKeyAttParse( const CK_ATTRIBUTE * pxAttribute,
                                      const mbedtls_pk_context * pxMbedContext )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        CK_BBOOL xBool = ( CK_BBOOL ) CK_FALSE;
        int32_t lMbedTLSResult = 0;
        CK_RV xResult = CKR_OK;

        mbedtls_ecp_keypair * pxKeyPair = ( mbedtls_ecp_keypair * ) pxMbedContext->pk_ctx;

        if( pxAttribute->type == CKA_VERIFY )
        {
            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, pxAttribute->ulValueLen );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool == ( CK_BBOOL ) CK_FALSE )
            {
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
                LogError( ( "Failed to parse EC public key. Expected verify permissions to be supported." ) );
            }
        }
        else
        {
            /* Strip the ANS.1 Encoding of type and length. Otherwise mbed TLS
             * won't be able to parse the binary EC point. */
            if( pxAttribute->ulValueLen >= 2UL )
            {
                lMbedTLSResult = mbedtls_ecp_point_read_binary( &pxKeyPair->grp,
                                                                &pxKeyPair->Q,
                                                                ( ( uint8_t * ) ( pxAttribute->pValue ) + 2U ),
                                                                ( pxAttribute->ulValueLen - 2U ) );

                if( lMbedTLSResult != 0 )
                {
                    xResult = CKR_FUNCTION_FAILED;
                    LogError( ( "Failed to parse EC public key. mbedtls_ecp_point_read_binary failed: mbed TLS error = %s : %s.",
                                mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                }
            }
            else
            {
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            }
        }

        return xResult;
    }
#endif /* ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM */

/**
 * @brief Parses attribute values for an EC Key.
 */
#ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM
    static CK_RV prvEcKeyAttParse( const CK_ATTRIBUTE * pxAttribute,
                                   const mbedtls_pk_context * pxMbedContext,
                                   CK_BBOOL xIsPrivate )
    {
        CK_RV xResult = CKR_OK;
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        CK_BBOOL xBool = ( CK_BBOOL ) CK_FALSE;
        const CK_BYTE pxEcCurve[] = pkcs11DER_ENCODED_OID_P256;
        const CK_BYTE * pxEcAttVal = NULL;
        const CK_BBOOL * pxEcBoolAtt = NULL;

        /* Common EC key attributes. */
        switch( pxAttribute->type )
        {
            case ( CKA_CLASS ):
            case ( CKA_KEY_TYPE ):
            case ( CKA_LABEL ):
                break;

            case ( CKA_TOKEN ):
                pxEcBoolAtt = ( CK_BBOOL * ) pxAttribute->pValue;

                if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
                {
                    ( void ) memcpy( &xBool, pxEcBoolAtt, sizeof( CK_BBOOL ) );
                }

                /* MISRA Ref 10.5.1 [Essential type casting] */
                /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                /* coverity[misra_c_2012_rule_10_5_violation] */
                if( xBool != ( CK_BBOOL ) CK_TRUE )
                {
                    LogError( ( "Failed parsing EC key template. Expected token type to be true, but it was false." ) );
                    xResult = CKR_ATTRIBUTE_VALUE_INVALID;
                }

                break;

            case ( CKA_EC_PARAMS ):
                pxEcAttVal = ( CK_BYTE * ) pxAttribute->pValue;

                if( pxAttribute->ulValueLen == sizeof( pxEcCurve ) )
                {
                    if( memcmp( pxEcCurve, pxEcAttVal, sizeof( pxEcCurve ) ) != 0 )
                    {
                        xResult = CKR_TEMPLATE_INCONSISTENT;
                        LogError( ( "Failed parsing EC key template. The elliptic curve was wrong. Expected elliptic curve P-256." ) );
                    }
                }

                break;

            case ( CKA_VERIFY ):
            case ( CKA_EC_POINT ):

                /* MISRA Ref 10.5.1 [Essential type casting] */
                /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                /* coverity[misra_c_2012_rule_10_5_violation] */
                if( xIsPrivate == ( CK_BBOOL ) CK_FALSE )
                {
                    xResult = prvEcPubKeyAttParse( pxAttribute, pxMbedContext );
                }
                else
                {
                    LogError( ( "Failed parsing EC key template. The key type "
                                "did not match the template parameters. Expected "
                                "a public key for CKA_VERIFY or CKA_EC_POINT." ) );
                    xResult = CKR_ATTRIBUTE_VALUE_INVALID;
                }

                break;

            case ( CKA_SIGN ):
            case ( CKA_VALUE ):

                /* MISRA Ref 10.5.1 [Essential type casting] */
                /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                /* coverity[misra_c_2012_rule_10_5_violation] */
                if( xIsPrivate == ( CK_BBOOL ) CK_TRUE )
                {
                    xResult = prvEcPrivKeyAttParse( pxAttribute, pxMbedContext );
                }
                else
                {
                    LogError( ( "Failed parsing EC key template. The key type "
                                "did not match the template parameters. Expected "
                                "a private key for CKA_SIGN or CKA_VALUE." ) );
                    xResult = CKR_ATTRIBUTE_VALUE_INVALID;
                }

                break;

            default:
                LogError( ( "Failed parsing EC key template. Unknown attribute "
                            "0x%0lX found for an EC key.", ( unsigned long int ) pxAttribute->type ) );
                xResult = CKR_ATTRIBUTE_TYPE_INVALID;
                break;
        }

        return xResult;
    }
#endif /* ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM */

/*-----------------------------------------------------------------------*/
/* Functions for maintaining the PKCS #11 module's label-handle lookups. */
/*-----------------------------------------------------------------------*/


/**
 * @brief Searches the PKCS #11 module's object list for label and provides handle.
 *
 * @param[in] pcLabel            Array containing label.
 * @param[in] xLabelLength       Length of the label, in bytes.
 * @param[out] pxPalHandle       Pointer to the PAL handle to be provided.
 *                               CK_INVALID_HANDLE if no object found.
 * @param[out] pxAppHandle       Pointer to the application handle to be provided.
 *                               CK_INVALID_HANDLE if no object found.
 */
static void prvFindObjectInListByLabel( const CK_BYTE * pcLabel,
                                        CK_ULONG xLabelLength,
                                        CK_OBJECT_HANDLE_PTR pxPalHandle,
                                        CK_OBJECT_HANDLE_PTR pxAppHandle )
{
    uint32_t ulIndex;

    *pxPalHandle = CK_INVALID_HANDLE;
    *pxAppHandle = CK_INVALID_HANDLE;

    for( ulIndex = 0; ulIndex < pkcs11configMAX_NUM_OBJECTS; ulIndex++ )
    {
        if( 0 == memcmp( pcLabel, xP11Context.xObjectList.xObjects[ ulIndex ].xLabel, xLabelLength ) )
        {
            LogDebug( ( "Found object in object list matching label." ) );
            *pxPalHandle = xP11Context.xObjectList.xObjects[ ulIndex ].xHandle;
            *pxAppHandle = ulIndex + 1UL; /* Zero is not a valid handle, so let's offset by 1. */
            break;
        }
    }
}

/**
 * @brief Looks up a PKCS #11 object's label and PAL handle given an application handle.
 *
 * @param[in] xAppHandle         The handle of the object being looked up for, used by the application.
 * @param[out] pxPalHandle        Pointer to the handle corresponding to xPalHandle being used by the PAL.
 * @param[out] ppcLabel          Pointer to an array containing label.  NULL if object not found.
 * @param[out] pxLabelLength     Pointer to label length (includes a string null terminator).
 *                               0 if no object found.
 */
static void prvFindObjectInListByHandle( CK_OBJECT_HANDLE xAppHandle,
                                         CK_OBJECT_HANDLE_PTR pxPalHandle,
                                         CK_BYTE_PTR * ppcLabel,
                                         CK_ULONG_PTR pxLabelLength )
{
    CK_OBJECT_HANDLE ulIndex = xAppHandle - ( ( CK_OBJECT_HANDLE ) 1 );

    *ppcLabel = NULL;
    *pxLabelLength = 0;
    *pxPalHandle = CK_INVALID_HANDLE;

    if( ulIndex < pkcs11configMAX_NUM_OBJECTS )
    {
        if( xP11Context.xObjectList.xObjects[ ulIndex ].xHandle != CK_INVALID_HANDLE )
        {
            LogDebug( ( "Found object in list by handle." ) );
            *ppcLabel = xP11Context.xObjectList.xObjects[ ulIndex ].xLabel;
            *pxLabelLength = xP11Context.xObjectList.xObjects[ ulIndex ].xLabelSize;
            *pxPalHandle = xP11Context.xObjectList.xObjects[ ulIndex ].xHandle;
        }
    }
}

/**
 * @brief Removes an object from the module object list (xP11Context.xObjectList)
 *
 * @warning This does not delete the object from NVM.
 *
 * @param[in] xPalHandle            PAL handle of the object to be deleted.
 */
static CK_RV prvDeleteObjectFromList( CK_OBJECT_HANDLE xPalHandle )
{
    CK_RV xResult = CKR_OK;
    int32_t lGotSemaphore = ( int32_t ) 0;
    CK_ULONG ulIndex;

    lGotSemaphore = mbedtls_mutex_lock( &xP11Context.xObjectList.xMutex );

    if( lGotSemaphore == 0 )
    {
        /* Remove all references that have the same PAL handle, as it has now
         * been deleted in the PKCS11_PAL. */
        for( ulIndex = 0; ulIndex < pkcs11configMAX_NUM_OBJECTS; ulIndex++ )
        {
            if( xP11Context.xObjectList.xObjects[ ulIndex ].xHandle == xPalHandle )
            {
                ( void ) memset( &xP11Context.xObjectList.xObjects[ ulIndex ], 0, sizeof( P11Object_t ) );
            }
        }

        ( void ) mbedtls_mutex_unlock( &xP11Context.xObjectList.xMutex );
    }
    else
    {
        LogError( ( "Failed to remove an object from internal object list. "
                    "Could not take the xObjectList mutex." ) );
        xResult = CKR_CANT_LOCK;
    }

    return xResult;
}

/**
 * @brief Add an object that exists in NVM to the application object array.
 *
 * @param[in] xPalHandle         The handle used by the PKCS #11 PAL for object.
 * @param[out] pxAppHandle       Updated to contain the application handle corresponding to xPalHandle.
 * @param[in]  pcLabel           Pointer to object label.
 * @param[in] xLabelLength       Length of the PKCS #11 label.
 *
 */
static CK_RV prvAddObjectToList( CK_OBJECT_HANDLE xPalHandle,
                                 CK_OBJECT_HANDLE_PTR pxAppHandle,
                                 const CK_BYTE * pcLabel,
                                 CK_ULONG xLabelLength )
{
    CK_RV xResult = CKR_HOST_MEMORY;

    if( xLabelLength > pkcs11configMAX_LABEL_LENGTH )
    {
        xResult = CKR_ARGUMENTS_BAD;
        LogError( ( "Failed to add object to internal object list: "
                    "xLabelLength exceeds pkcs11configMAX_LABEL_LENGTH." ) );
    }
    else if( 0 == mbedtls_mutex_lock( &xP11Context.xObjectList.xMutex ) )
    {
        CK_ULONG ulSearchIndex;
        CK_ULONG ulEmptyIndex = 0;
        P11Object_t * pxEmptyP11Object = NULL;

        /* Iterate over list to find an existing entry containing xPalHandle */
        for( ulSearchIndex = 0; ulSearchIndex < pkcs11configMAX_NUM_OBJECTS; ulSearchIndex++ )
        {
            P11Object_t * pxP11Object = &( xP11Context.xObjectList.xObjects[ ulSearchIndex ] );

            /* Update an existing entry with the desired xPalHandle */
            if( pxP11Object->xHandle == xPalHandle )
            {
                ( void ) memcpy( pxP11Object->xLabel, pcLabel, xLabelLength );
                pxP11Object->xLabelSize = xLabelLength;

                xResult = CKR_OK;
                *pxAppHandle = ulSearchIndex + 1;

                /* Entry updated, so exit the loop. */
                break;
            }
            else
            {
                if( ( pxP11Object->xHandle == CK_INVALID_HANDLE ) &&
                    ( pxEmptyP11Object == NULL ) )
                {
                    pxEmptyP11Object = pxP11Object;
                    ulEmptyIndex = ulSearchIndex;
                }
            }
        }

        /* Check if we have reached the end of the list without writing */
        if( ( xResult != CKR_OK ) &&
            ( pxEmptyP11Object != NULL ) )
        {
            pxEmptyP11Object->xHandle = xPalHandle;
            pxEmptyP11Object->xLabelSize = xLabelLength;

            ( void ) memcpy( pxEmptyP11Object->xLabel, pcLabel, xLabelLength );

            *pxAppHandle = ulEmptyIndex + 1;
            xResult = CKR_OK;
        }

        ( void ) mbedtls_mutex_unlock( &xP11Context.xObjectList.xMutex );
    }
    else
    {
        LogError( ( "Failed to add object to internal object list. Could not "
                    "take xObjectList mutex." ) );
        xResult = CKR_CANT_LOCK;
    }

    return xResult;
}

/**
 * @brief Append an empty public key to DER formatted EC private key.
 */
static CK_RV prvAppendEmptyECDerKey( uint8_t * pusECPrivateKey,
                                     uint32_t ulDerBufSize,
                                     int32_t lDerKeyLength,
                                     uint32_t * pulActualKeyLength )
{
    CK_RV xResult = CKR_OK;
    const uint8_t emptyPubKey[ 6 ] = { 0xa1, 0x04, 0x03, 0x02, 0x00, 0x00 };
    int32_t lCompare = 0;

    /*
     * mbedtls_pk_write_key_der appends empty public
     * key data when saving EC private key
     * that does not have a public key associated with it.
     * a1 04        -> Application identifier of length 4
     * 03 02     -> Bit string of length 2
     *    00 00  -> "Public key"
     * https://forums.mbed.com/t/how-do-i-write-an-ec-private-key-w-no-public-key-to-der-format/4728 */

    /* If there was no public key in the structure, this byte
     * array will be appended to the valid private key.
     * It must be removed so that we can read the private
     * key back at a later time. */
    lCompare = memcmp( &( pusECPrivateKey[ ulDerBufSize - 6UL ] ), emptyPubKey, sizeof( emptyPubKey ) );

    if( ( lCompare == 0 ) && ( *pulActualKeyLength >= 6UL ) )
    {
        /* Do not write the last 6 bytes to key storage. */
        pusECPrivateKey[ ulDerBufSize - ( uint32_t ) lDerKeyLength + ( ( uint32_t ) 1 ) ] -= ( uint8_t ) 6;
        *pulActualKeyLength -= ( ( uint32_t ) 6 );
    }

    return xResult;
}

/**
 * @brief Save a DER formatted key in the PKCS #11 PAL.
 */
static CK_RV prvSaveDerKeyToPal( mbedtls_pk_context * pxMbedContext,
                                 CK_OBJECT_HANDLE_PTR pxObject,
                                 CK_ATTRIBUTE * pxLabel,
                                 CK_KEY_TYPE xKeyType,
                                 CK_BBOOL xIsPrivate )
{
    CK_RV xResult = CKR_OK;
    CK_BYTE_PTR pxDerKey = NULL;
    int32_t lDerKeyLength = 0;
    uint32_t ulActualKeyLength = 0;
    CK_OBJECT_HANDLE xPalHandle = CK_INVALID_HANDLE;
    uint32_t ulDerBufSize = 0;

    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    if( xIsPrivate == ( CK_BBOOL ) CK_TRUE )
    {
        LogDebug( ( "Key was private type." ) );

        if( xKeyType == CKK_EC )
        {
            LogDebug( ( "Received EC key type." ) );
            ulDerBufSize = pkcs11_MAX_EC_PRIVATE_KEY_DER_SIZE;
        }
        else
        {
            LogDebug( ( "Received RSA key type." ) );
            ulDerBufSize = pkcs11_MAX_PRIVATE_KEY_DER_SIZE;
        }
    }
    else
    {
        LogDebug( ( "Key was public type." ) );

        if( xKeyType == CKK_EC )
        {
            LogDebug( ( "Received EC key type." ) );
            ulDerBufSize = pkcs11_MAX_EC_PUBLIC_KEY_DER_SIZE;
        }
        else
        {
            LogDebug( ( "Received RSA key type." ) );
            ulDerBufSize = pkcs11_PUBLIC_RSA_2048_DER_SIZE;
        }
    }

    LogDebug( ( "Allocating a %lu bytes sized buffer to write the key to.", ( unsigned long int ) ulDerBufSize ) );
    pxDerKey = mbedtls_calloc( 1, ulDerBufSize );

    if( pxDerKey == NULL )
    {
        LogError( ( "Failed saving DER formatted key to flash. Failed to malloc a buffer to contain the key for the mbed TLS context." ) );
        xResult = CKR_HOST_MEMORY;
    }
    else
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
/* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        if( xIsPrivate == ( CK_BBOOL ) CK_TRUE )
        {
            lDerKeyLength = mbedtls_pk_write_key_der( pxMbedContext, pxDerKey, ulDerBufSize );
        }
        else
        {
            lDerKeyLength = mbedtls_pk_write_pubkey_der( pxMbedContext, pxDerKey, ulDerBufSize );
        }
    }

    if( lDerKeyLength < 0 )
    {
        LogError( ( "Failed saving DER formatted key to flash. mbed TLS pk_write failed: mbed TLS error = %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( lDerKeyLength ),
                    mbedtlsLowLevelCodeOrDefault( lDerKeyLength ) ) );
        xResult = CKR_FUNCTION_FAILED;
    }
    else
    {
        /* Cast to unsigned int as the result was not negative. */
        ulActualKeyLength = ( uint32_t ) lDerKeyLength;
    }

    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    if( ( xResult == CKR_OK ) && ( xIsPrivate == ( CK_BBOOL ) CK_TRUE ) && ( xKeyType == CKK_EC ) )
    {
        xResult = prvAppendEmptyECDerKey( pxDerKey, ulDerBufSize, lDerKeyLength, &ulActualKeyLength );
    }

    if( ( xResult == CKR_OK ) && ( lDerKeyLength > 0 ) && ( ( uint32_t ) lDerKeyLength < ulDerBufSize ) )
    {
        xPalHandle = PKCS11_PAL_SaveObject( pxLabel,
                                            pxDerKey + ( ulDerBufSize - ( uint32_t ) lDerKeyLength ),
                                            ulActualKeyLength );

        if( xPalHandle == CK_INVALID_HANDLE )
        {
            LogError( ( "Failed saving DER formatted key to flash. Failed to write DER formatted key to the PKCS #11 PAL." ) );
            xResult = CKR_DEVICE_MEMORY;
        }
    }

    if( xResult == CKR_OK )
    {
        xResult = prvAddObjectToList( xPalHandle, pxObject, pxLabel->pValue, pxLabel->ulValueLen );
    }

    mbedtls_free( pxDerKey );

    return xResult;
}

/*-------------------------------------------------------------*/

/**
 * @brief Initializes Cryptoki.
 *
 * @note C_Initialize is not thread-safe.
 *
 * C_Initialize should be called (and allowed to return) before
 * any additional PKCS #11 operations are invoked.
 *
 * In this implementation, all arguments are ignored.
 * Thread protection for the rest of PKCS #11 functions
 * default to FreeRTOS primitives.
 *
 * @param[in] pInitArgs    This parameter is ignored.
 *
 * @return CKR_OK if successful.
 * CKR_CRYPTOKI_ALREADY_INITIALIZED if C_Initialize was previously called.
 * All other errors indicate that the PKCS #11 module is not ready to be used.
 */
/* @[declare_pkcs11_mbedtls_c_initialize] */
CK_DECLARE_FUNCTION( CK_RV, C_Initialize )( CK_VOID_PTR pInitArgs )
{
    CK_RV xResult = CKR_OK;

    ( void ) ( pInitArgs );

    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    if( xP11Context.xIsInitialized != ( CK_BBOOL ) CK_TRUE )
    {
        xResult = PKCS11_PAL_Initialize();

        if( xResult == CKR_OK )
        {
            xResult = prvMbedTLS_Initialize();
        }
        else
        {
            LogError( ( "Failed to initialize PKCS #11. PAL failed with error code: 0x%0lX", ( unsigned long int ) xResult ) );
        }
    }
    else
    {
        xResult = CKR_CRYPTOKI_ALREADY_INITIALIZED;
        LogWarn( ( "Failed to initialize PKCS #11. PKCS #11 was already initialized." ) );
    }

    if( xResult == CKR_OK )
    {
        LogInfo( ( "PKCS #11 successfully initialized." ) );
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_initialize] */

/**
 * @brief Clean up miscellaneous Cryptoki-associated resources.
 */
/* @[declare_pkcs11_mbedtls_c_finalize] */
CK_DECLARE_FUNCTION( CK_RV, C_Finalize )( CK_VOID_PTR pReserved )
{
    CK_RV xResult = CKR_OK;

    if( pReserved != NULL )
    {
        xResult = CKR_ARGUMENTS_BAD;
        LogError( ( "Failed to un-initialize PKCS #11. Received bad arguments. "
                    "Parameters must be NULL, but were not." ) );
    }

    if( xResult == CKR_OK )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        if( xP11Context.xIsInitialized == ( CK_BBOOL ) CK_FALSE )
        {
            xResult = CKR_CRYPTOKI_NOT_INITIALIZED;
            LogWarn( ( "PKCS #11 was already uninitialized." ) );
        }
    }

    if( xResult == CKR_OK )
    {
        mbedtls_entropy_free( &xP11Context.xMbedEntropyContext );
        mbedtls_ctr_drbg_free( &xP11Context.xMbedDrbgCtx );
        mbedtls_mutex_free( &xP11Context.xObjectList.xMutex );
        mbedtls_mutex_free( &xP11Context.xSessionMutex );

        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        xP11Context.xIsInitialized = ( CK_BBOOL ) CK_FALSE;
        LogInfo( ( "PKCS #11 was successfully uninitialized." ) );
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_finalize] */

/**
 * @brief Obtains entry points of Cryptoki library functions.
 *
 * All other PKCS #11 functions should be invoked using the returned
 * function list.
 *
 * @warning Do not overwrite the function list.
 *
 * \param[in] ppFunctionList       Pointer to the location where
 *                                  pointer to function list will be placed.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_getfunctionlist] */
CK_DECLARE_FUNCTION( CK_RV, C_GetFunctionList )( CK_FUNCTION_LIST_PTR_PTR ppFunctionList )
{
    CK_RV xResult = CKR_OK;

    static CK_FUNCTION_LIST prvP11FunctionList =
    {
        { CRYPTOKI_VERSION_MAJOR, CRYPTOKI_VERSION_MINOR },
        C_Initialize,
        C_Finalize,
        NULL, /*C_GetInfo */
        C_GetFunctionList,
        C_GetSlotList,
        NULL, /*C_GetSlotInfo*/
        C_GetTokenInfo,
        NULL, /*C_GetMechanismList*/
        C_GetMechanismInfo,
        C_InitToken,
        NULL, /*C_InitPIN*/
        NULL, /*C_SetPIN*/
        C_OpenSession,
        C_CloseSession,
        NULL,    /*C_CloseAllSessions*/
        NULL,    /*C_GetSessionInfo*/
        NULL,    /*C_GetOperationState*/
        NULL,    /*C_SetOperationState*/
        C_Login, /*C_Login*/
        NULL,    /*C_Logout*/
        C_CreateObject,
        NULL,    /*C_CopyObject*/
        C_DestroyObject,
        NULL,    /*C_GetObjectSize*/
        C_GetAttributeValue,
        NULL,    /*C_SetAttributeValue*/
        C_FindObjectsInit,
        C_FindObjects,
        C_FindObjectsFinal,
        NULL, /*C_EncryptInit*/
        NULL, /*C_Encrypt*/
        NULL, /*C_EncryptUpdate*/
        NULL, /*C_EncryptFinal*/
        NULL, /*C_DecryptInit*/
        NULL, /*C_Decrypt*/
        NULL, /*C_DecryptUpdate*/
        NULL, /*C_DecryptFinal*/
        C_DigestInit,
        NULL, /*C_Digest*/
        C_DigestUpdate,
        NULL, /* C_DigestKey*/
        C_DigestFinal,
        C_SignInit,
        C_Sign,
        NULL, /*C_SignUpdate*/
        NULL, /*C_SignFinal*/
        NULL, /*C_SignRecoverInit*/
        NULL, /*C_SignRecover*/
        C_VerifyInit,
        C_Verify,
        NULL, /*C_VerifyUpdate*/
        NULL, /*C_VerifyFinal*/
        NULL, /*C_VerifyRecoverInit*/
        NULL, /*C_VerifyRecover*/
        NULL, /*C_DigestEncryptUpdate*/
        NULL, /*C_DecryptDigestUpdate*/
        NULL, /*C_SignEncryptUpdate*/
        NULL, /*C_DecryptVerifyUpdate*/
        NULL, /*C_GenerateKey*/
        C_GenerateKeyPair,
        NULL, /*C_WrapKey*/
        NULL, /*C_UnwrapKey*/
        NULL, /*C_DeriveKey*/
        NULL, /*C_SeedRandom*/
        C_GenerateRandom,
        NULL, /*C_GetFunctionStatus*/
        NULL, /*C_CancelFunction*/
        NULL  /*C_WaitForSlotEvent*/
    };

    if( NULL == ppFunctionList )
    {
        xResult = CKR_ARGUMENTS_BAD;
        LogError( ( "Failed to return function pointer list. Expected a valid "
                    "pointer to a CK_FUNCTION_LIST, but the pointer was NULL." ) );
    }
    else
    {
        *ppFunctionList = &prvP11FunctionList;
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_getfunctionlist] */

/**
 * @brief Obtains a list of slots in the system.
 *
 * This port does not implement the concept of separate slots/tokens.
 *
 *  \param[in]  tokenPresent   This parameter is unused by this port.
 *  \param[in]  pSlotList      Pointer to an array of slot IDs.
 *                              At this time, only 1 slot is implemented.
 *  \param[in,out]  pulCount    Length of the slot list pxSlotList. Updated
 *                              to contain the actual number of slots written
 *                              to the list.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_getslotlist] */
CK_DECLARE_FUNCTION( CK_RV, C_GetSlotList )( CK_BBOOL tokenPresent,
                                             CK_SLOT_ID_PTR pSlotList,
                                             CK_ULONG_PTR pulCount )
{
    CK_RV xResult = CKR_OK;

    /* Since the mbedTLS implementation of PKCS#11 does not depend
     * on a physical token, this parameter is ignored. */
    ( void ) ( tokenPresent );

    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    if( xP11Context.xIsInitialized != ( CK_BBOOL ) CK_TRUE )
    {
        xResult = CKR_CRYPTOKI_NOT_INITIALIZED;
        LogError( ( "Failed to get slot list. PKCS #11 must be initialized "
                    "before any operations." ) );
    }

    if( NULL == pulCount )
    {
        xResult = CKR_ARGUMENTS_BAD;
        LogError( ( "Failed to get slot list. Count pointer was NULL." ) );
    }

    if( xResult == CKR_OK )
    {
        if( NULL == pSlotList )
        {
            *pulCount = 1;
        }
        else
        {
            if( 0u == *pulCount )
            {
                xResult = CKR_BUFFER_TOO_SMALL;
                LogWarn( ( "The buffer is too small to contain the slot list." ) );
            }
            else
            {
                pSlotList[ 0 ] = pkcs11SLOT_ID;
                *pulCount = 1;
                LogDebug( ( "Successfully Returned a PKCS #11 slot with ID "
                            "%lu with a count of %lu.", ( unsigned long int ) pkcs11SLOT_ID, ( unsigned long int ) *pulCount ) );
            }
        }
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_getslotlist] */


/**
 * @brief Obtains information about a particular token.
 *
 * @param[in]  slotID         This parameter is unused in this port.
 * @param[out] pInfo           This parameter is unused in this port.
 *
 * C_GetTokenInfo() is only implemented for compatibility with other ports.
 * All inputs to this function are ignored, and calling this
 * function on this port does provide any information about
 * the PKCS #11 token.
 *
 * @return CKR_OK.
 */
/* @[declare_pkcs11_mbedtls_c_gettokeninfo] */
CK_DECLARE_FUNCTION( CK_RV, C_GetTokenInfo )( CK_SLOT_ID slotID,
                                              CK_TOKEN_INFO_PTR pInfo )
{
    /* Avoid compiler warnings about unused variables. */
    ( void ) slotID;
    ( void ) pInfo;

    LogWarn( ( "C_GetTokenInfo is not implemented." ) );

    return CKR_OK;
}
/* @[declare_pkcs11_mbedtls_c_gettokeninfo] */

/**
 * @brief Obtains information about a particular mechanism.
 *
 *  \param[in]  slotID         This parameter is unused in this port.
 *  \param[in]  type            The cryptographic capability for which support
 *                              information is being queried.
 *  \param[out] pInfo           Algorithm sizes and flags for the requested
 *                              mechanism, if supported.
 *
 * @return CKR_OK if the mechanism is supported. Otherwise, CKR_MECHANISM_INVALID.
 */
/* @[declare_pkcs11_mbedtls_c_getmechanisminfo] */
CK_DECLARE_FUNCTION( CK_RV, C_GetMechanismInfo )( CK_SLOT_ID slotID,
                                                  CK_MECHANISM_TYPE type,
                                                  CK_MECHANISM_INFO_PTR pInfo )
{
    CK_RV xResult = CKR_MECHANISM_INVALID;

    struct CryptoMechanisms
    {
        CK_MECHANISM_TYPE xType;
        CK_MECHANISM_INFO xInfo;
    }
    pxSupportedMechanisms[] =
    {
        { CKM_RSA_PKCS,        { 2048, 2048, CKF_SIGN              } },
        { CKM_RSA_X_509,       { 2048, 2048, CKF_VERIFY            } },
        #ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM
        { CKM_ECDSA,           { 256,  256,  CKF_SIGN | CKF_VERIFY } },
        { CKM_EC_KEY_PAIR_GEN, { 256,  256,  CKF_GENERATE_KEY_PAIR } },
        #endif
        { CKM_SHA256,          { 0,    0,    CKF_DIGEST            } }
    };
    uint32_t ulMech = 0;

    ( void ) slotID;

    if( pInfo == NULL )
    {
        xResult = CKR_ARGUMENTS_BAD;
        LogError( ( "Failed to get mechanism info. pInfo was NULL. Expected a "
                    "pointer to a valid CK_MECHANISM_INFO struct." ) );
    }
    else
    {
        /* Look for the requested mechanism in the above table. */
        for( ; ulMech < sizeof( pxSupportedMechanisms ) / sizeof( pxSupportedMechanisms[ 0 ] ); ulMech++ )
        {
            if( pxSupportedMechanisms[ ulMech ].xType == type )
            {
                /* The mechanism is supported. Copy out the details and break
                 * out of the loop. */
                ( void ) memcpy( pInfo, &( pxSupportedMechanisms[ ulMech ].xInfo ), sizeof( CK_MECHANISM_INFO ) );
                xResult = CKR_OK;
                break;
            }
        }
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_getmechanisminfo] */

/**
 * @brief Initializes a token. This function is not implemented for this port.
 *
 * C_InitToken() is only implemented for compatibility with other ports.
 * All inputs to this function are ignored, and calling this
 * function on this port does not add any security.
 *
 * @return CKR_OK.
 */
/* @[declare_pkcs11_mbedtls_c_inittoken] */
CK_DECLARE_FUNCTION( CK_RV, C_InitToken )( CK_SLOT_ID slotID,
                                           CK_UTF8CHAR_PTR pPin,
                                           CK_ULONG ulPinLen,
                                           CK_UTF8CHAR_PTR pLabel )
{
    /* Avoid compiler warnings about unused variables. */
    ( void ) slotID;
    ( void ) pPin;
    ( void ) ulPinLen;
    ( void ) pLabel;

    LogWarn( ( "C_InitToken is not implemented." ) );

    return CKR_OK;
}
/* @[declare_pkcs11_mbedtls_c_inittoken] */

/**
 * @brief Opens a connection between an application and a particular token or sets up an application callback for token insertion.
 *
 * \note PKCS #11 module must have been previously initialized with a call to
 * C_Initialize() before calling C_OpenSession().
 *
 *
 *  \param[in]  slotID          This parameter is unused in this port.
 *  \param[in]  flags           Session flags - CKF_SERIAL_SESSION is a
 *                              mandatory flag.
 *  \param[in]  pApplication    This parameter is unused in this port.
 *  \param[in]  Notify          This parameter is unused in this port.
 *  \param[in]  phSession       Pointer to the location that the created
 *                              session's handle will be placed.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_opensession] */
CK_DECLARE_FUNCTION( CK_RV, C_OpenSession )( CK_SLOT_ID slotID,
                                             CK_FLAGS flags,
                                             CK_VOID_PTR pApplication,
                                             CK_NOTIFY Notify,
                                             CK_SESSION_HANDLE_PTR phSession )
{
    CK_RV xResult = CKR_OK;
    P11Session_t * pxSessionObj = NULL;
    uint32_t ulSessionCount = 0;

    ( void ) ( slotID );
    ( void ) ( pApplication );

    /* Allow unused parameters to be cast to void to silence compiler warnings.
     * Even if they are a function pointer. */
    /* MISRA Ref 11.1.1 [Object type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-111 */
    ( void ) Notify;

    /* Check that the PKCS #11 module is initialized. */
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    if( xP11Context.xIsInitialized != ( CK_BBOOL ) CK_TRUE )
    {
        xResult = CKR_CRYPTOKI_NOT_INITIALIZED;
        LogError( ( "Could not open a session. PKCS #11 must be initialized "
                    "before any operations." ) );
    }

    /* Check arguments. */
    if( NULL == phSession )
    {
        xResult = CKR_ARGUMENTS_BAD;
        LogError( ( "Could not open a session. phSession cannot be a NULL pointer." ) );
    }

    /* For legacy reasons, the CKF_SERIAL_SESSION bit MUST always be set. */
    if( ( CKR_OK == xResult ) && ( 0UL == ( CKF_SERIAL_SESSION & flags ) ) )
    {
        xResult = CKR_SESSION_PARALLEL_NOT_SUPPORTED;
        LogError( ( "Could not open a session. CKR_SESSION_PARALLEL_NOT_SUPPORTED "
                    "must always be a set flag." ) );
    }

    /*
     * Make space for the context.
     */
    if( CKR_OK == xResult )
    {
        /* Get next open session slot. */
        if( mbedtls_mutex_lock( &xP11Context.xSessionMutex ) == 0 )
        {
            for( ulSessionCount = 0; ulSessionCount < pkcs11configMAX_SESSIONS; ++ulSessionCount )
            {
                /* MISRA Ref 10.5.1 [Essential type casting] */
                /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                /* coverity[misra_c_2012_rule_10_5_violation] */
                if( pxP11Sessions[ ulSessionCount ].xOpened == ( CK_BBOOL ) CK_FALSE )
                {
                    xResult = CKR_OK;
                    pxSessionObj = &( pxP11Sessions[ ulSessionCount ] );
                    /* MISRA Ref 10.5.1 [Essential type casting] */
                    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                    /* coverity[misra_c_2012_rule_10_5_violation] */
                    pxSessionObj->xOpened = ( CK_BBOOL ) CK_TRUE;
                    break;
                }
                else
                {
                    xResult = CKR_SESSION_COUNT;
                }
            }

            ( void ) mbedtls_mutex_unlock( &xP11Context.xSessionMutex );
        }
        else
        {
            xResult = CKR_FUNCTION_FAILED;
            LogError( ( "Could not open a session. Unsuccessful in taking xSessionMutex." ) );
        }

        if( CKR_OK == xResult )
        {
            mbedtls_mutex_init( &pxSessionObj->xSignMutex );
            mbedtls_mutex_init( &pxSessionObj->xVerifyMutex );
        }
    }

    if( CKR_OK == xResult )
    {
        /*
         * Assign the session.
         */
        pxSessionObj->ulState =
            ( 0UL != ( flags & CKF_RW_SESSION ) ) ? CKS_RW_PUBLIC_SESSION : CKS_RO_PUBLIC_SESSION;
        LogDebug( ( "Assigned a 0x%0lX Type Session.", ( unsigned long int ) pxSessionObj->ulState ) );
    }

    /*
     *   Initialize the operation in progress.
     */
    if( CKR_OK == xResult )
    {
        pxSessionObj->xOperationDigestMechanism = pkcs11NO_OPERATION;
        pxSessionObj->xOperationVerifyMechanism = pkcs11NO_OPERATION;
        pxSessionObj->xOperationSignMechanism = pkcs11NO_OPERATION;
        LogDebug( ( "Assigned Mechanisms to no operation in progress." ) );
    }

    if( xResult == CKR_SESSION_COUNT )
    {
        /* No available session. */
        LogError( ( "Could not open a session. All sessions have "
                    "been taken. Consider increasing value of "
                    "pkcs11configMAX_SESSIONS." ) );
    }

    if( CKR_OK == xResult )
    {
        /* Increment by one, as invalid handles in PKCS #11 are 0. */
        ++ulSessionCount;
        *phSession = ulSessionCount;
        LogDebug( ( "Current session count at %lu", ( unsigned long int ) ( ulSessionCount - 1UL ) ) );
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_opensession] */

/**
 * @brief Closes a session.
 *
 * @param[in]   hSession        The session handle to
 *                              be terminated.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_closesession] */
CK_DECLARE_FUNCTION( CK_RV, C_CloseSession )( CK_SESSION_HANDLE hSession )
{
    P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = CKR_OK;

    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    if( xP11Context.xIsInitialized == ( CK_BBOOL ) CK_FALSE )
    {
        xResult = CKR_CRYPTOKI_NOT_INITIALIZED;
        LogError( ( "Could not close a session. PKCS #11 must be initialized "
                    "before any operations." ) );
    }
    else if( pxSession == NULL )
    {
        xResult = CKR_SESSION_HANDLE_INVALID;
        LogError( ( "Could not close a session. The PKCS #11 session handle "
                    "was invalid." ) );
    }
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    else if( pxSession->xOpened == ( CK_BBOOL ) CK_TRUE )
    {
        /*
         * Tear down the session.
         */
        mbedtls_pk_free( &pxSession->xSignKey );
        pxSession->xSignKeyHandle = CK_INVALID_HANDLE;
        mbedtls_mutex_free( &pxSession->xSignMutex );

        /* Free the public key context if it exists. */
        mbedtls_pk_free( &pxSession->xVerifyKey );
        pxSession->xVerifyKeyHandle = CK_INVALID_HANDLE;
        mbedtls_mutex_free( &pxSession->xVerifyMutex );

        mbedtls_sha256_free( &pxSession->xSHA256Context );

        /* memset clears the open flag, so there is no need to set it to CK_FALSE */
        ( void ) memset( pxSession, 0, sizeof( P11Session_t ) );
        LogInfo( ( "Successfully closed PKCS #11 session." ) );
    }
    else
    {
        /* MISRA */
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_closesession] */


/**
 * @brief Logs into a token. This function is not implemented for this port.
 *
 * C_Login() is only implemented for compatibility with other ports.
 * All inputs to this function are ignored, and calling this
 * function on this port does not add any security.
 *
 * @return CKR_OK.
 */
/* @[declare_pkcs11_mbedtls_c_login] */
CK_DECLARE_FUNCTION( CK_RV, C_Login )( CK_SESSION_HANDLE hSession,
                                       CK_USER_TYPE userType,
                                       CK_UTF8CHAR_PTR pPin,
                                       CK_ULONG ulPinLen )
{
    /* Avoid warnings about unused parameters. */
    ( void ) hSession;
    ( void ) userType;
    ( void ) pPin;
    ( void ) ulPinLen;

    LogDebug( ( "C_Login is not implemented." ) );

    /* THIS FUNCTION IS NOT IMPLEMENTED FOR MBEDTLS-BASED PORTS.
     * If login capability is required, implement it here.
     * Defined for compatibility with other PKCS #11 ports. */
    return CKR_OK;
}
/* @[declare_pkcs11_mbedtls_c_login] */

/**
 * @brief Helper function for parsing the templates of device certificates for C_CreateObject.
 *
 * @param[in] pxTemplate    Pointer to PKCS #11 attribute template.
 * @param[in] ulCount       length of templates array.
 * @param[in] pxObject      Pointer to PKCS #11 object.
 * @return CKR_OK.
 */
static CK_RV prvCreateCertificate( CK_ATTRIBUTE * pxTemplate,
                                   CK_ULONG ulCount,
                                   CK_OBJECT_HANDLE_PTR pxObject )
{
    CK_RV xResult = CKR_OK;
    CK_BYTE_PTR pxCertificateValue = NULL;
    CK_ULONG xCertificateLength = 0;
    CK_ATTRIBUTE_PTR pxLabel = NULL;
    CK_OBJECT_HANDLE xPalHandle = CK_INVALID_HANDLE;
    CK_CERTIFICATE_TYPE xCertificateType = 0;
    CK_ULONG ulIndex = 0;

    /* Search for the pointer to the certificate VALUE. */
    for( ulIndex = 0; ulIndex < ulCount; ulIndex++ )
    {
        xResult = prvCertAttParse( &( pxTemplate[ ulIndex ] ), &xCertificateType,
                                   &pxCertificateValue, &xCertificateLength,
                                   &pxLabel );

        if( xResult != CKR_OK )
        {
            break;
        }
    }

    if( ( xResult == CKR_OK ) && ( ( pxCertificateValue == NULL ) || ( pxLabel == NULL ) ) )
    {
        LogError( ( "Failed to create certificate. Could not parse the "
                    "certificate value or label from the template." ) );
        xResult = CKR_TEMPLATE_INCOMPLETE;
    }

    if( xResult == CKR_OK )
    {
        xPalHandle = PKCS11_PAL_SaveObject( pxLabel, pxCertificateValue, xCertificateLength );

        if( xPalHandle == 0UL ) /*Invalid handle. */
        {
            LogError( ( "Failed to create certificate. Could not save the "
                        "certificate to the PKCS #11 PAL." ) );
            xResult = CKR_DEVICE_MEMORY;
        }
    }

    if( xResult == CKR_OK )
    {
        xResult = prvAddObjectToList( xPalHandle, pxObject, pxLabel->pValue, pxLabel->ulValueLen );
    }

    return xResult;
}


/**
 * @brief Helper to search an attribute for the key type attribute.
 *
 * @param[out] pxKeyType pointer to key type.
 * @param[in] pxTemplate templates to search for a key in.
 * @param[in] ulCount length of templates array.
 */
static void prvGetKeyType( CK_KEY_TYPE * pxKeyType,
                           const CK_ATTRIBUTE * pxTemplate,
                           CK_ULONG ulCount )
{
    uint32_t ulIndex;
    CK_ATTRIBUTE xAttribute;

    *pxKeyType = PKCS11_INVALID_KEY_TYPE;

    for( ulIndex = 0; ulIndex < ulCount; ulIndex++ )
    {
        xAttribute = pxTemplate[ ulIndex ];

        if( ( xAttribute.type == CKA_KEY_TYPE ) && ( xAttribute.ulValueLen == sizeof( CK_KEY_TYPE ) ) )
        {
            LogDebug( ( "Successfully found the key type in the template." ) );
            ( void ) memcpy( pxKeyType, xAttribute.pValue, sizeof( CK_KEY_TYPE ) );
            break;
        }
    }
}

/**
 * @brief Helper to search a template for the label attribute.
 *
 * @param[out] ppxLabel pointer to label.
 * @param[in] pxTemplate templates to search for a key in.
 * @param[in] ulCount length of templates array.
 */
static void prvGetLabel( CK_ATTRIBUTE ** ppxLabel,
                         CK_ATTRIBUTE * pxTemplate,
                         CK_ULONG ulCount )
{
    CK_ATTRIBUTE xAttribute;
    CK_ULONG ulIndex;

    *ppxLabel = NULL;

    for( ulIndex = 0; ulIndex < ulCount; ulIndex++ )
    {
        xAttribute = pxTemplate[ ulIndex ];

        if( xAttribute.type == CKA_LABEL )
        {
            LogDebug( ( "Successfully found the label in the template." ) );
            *ppxLabel = &( pxTemplate[ ulIndex ] );
            break;
        }
    }
}

#ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM

/**
 * @brief Helper to search a template for the label attribute.
 *
 * @param[in] pxPalHandle opaque handle to PKCS #11 object.
 * @param[in] pxMbedContext mbedTLS pk context for parsing.
 * @param[in] pxLabel label of PKCS #11 object.
 *
 * @note Because public and private keys are stored in the same slot for this port,
 * importing one after the other requires a read of what was previously in the slot,
 * combination of the public and private key in DER format, and re-import of the
 * combination.
 */
    static CK_RV prvGetExistingKeyComponent( CK_OBJECT_HANDLE_PTR pxPalHandle,
                                             mbedtls_pk_context * pxMbedContext,
                                             const CK_ATTRIBUTE * pxLabel )
    {
        CK_BYTE_PTR pucData = NULL;
        CK_ULONG ulDataLength = 0;
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        CK_BBOOL xIsPrivate = ( CK_BBOOL ) CK_TRUE;
        CK_RV xResult = CKR_OK;
        int32_t lMbedTLSResult = 0;
        CK_BYTE pxPubKeyLabel[] = { pkcs11configLABEL_DEVICE_PUBLIC_KEY_FOR_TLS };
        CK_BYTE pxPrivKeyLabel[] = { pkcs11configLABEL_DEVICE_PRIVATE_KEY_FOR_TLS };

        *pxPalHandle = CK_INVALID_HANDLE;

        if( 0 == strncmp( pxLabel->pValue, pkcs11configLABEL_DEVICE_PRIVATE_KEY_FOR_TLS, pxLabel->ulValueLen ) )
        {
            *pxPalHandle = PKCS11_PAL_FindObject( pxPrivKeyLabel, pxLabel->ulValueLen );
        }
        else if( 0 == strncmp( pxLabel->pValue, pkcs11configLABEL_DEVICE_PUBLIC_KEY_FOR_TLS, pxLabel->ulValueLen ) )
        {
            *pxPalHandle = PKCS11_PAL_FindObject( pxPubKeyLabel, pxLabel->ulValueLen );
            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            xIsPrivate = ( CK_BBOOL ) CK_FALSE;
        }
        else
        {
            /* Unknown label passed to function */
            LogWarn( ( "Unknown label found." ) );
        }

        if( *pxPalHandle != CK_INVALID_HANDLE )
        {
            xResult = PKCS11_PAL_GetObjectValue( *pxPalHandle, &pucData, &ulDataLength, &xIsPrivate );
        }
        else
        {
            LogDebug( ( "Could not find an existing PKCS #11 PAL object." ) );
        }

        if( ( xResult == CKR_OK ) && ( *pxPalHandle != CK_INVALID_HANDLE ) )
        {
            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xIsPrivate == ( CK_BBOOL ) CK_TRUE )
            {
                #if MBEDTLS_VERSION_NUMBER < 0x03000000
                    lMbedTLSResult = mbedtls_pk_parse_key( pxMbedContext,
                                                           pucData, ulDataLength,
                                                           NULL, 0 );
                #else
                    lMbedTLSResult = mbedtls_pk_parse_key( pxMbedContext,
                                                           pucData, ulDataLength,
                                                           NULL, 0,
                                                           mbedtls_ctr_drbg_random, &xP11Context.xMbedDrbgCtx );
                #endif /* MBEDTLS_VERSION_NUMBER < 0x03000000 */
            }
            else
            {
                lMbedTLSResult = mbedtls_pk_parse_public_key( pxMbedContext, pucData, ulDataLength );
            }

            PKCS11_PAL_GetObjectValueCleanup( pucData, ulDataLength );
        }

        if( lMbedTLSResult != 0 )
        {
            LogError( ( "Failed to get existing object value. mbedTLS pk parse "
                        "failed with mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            *pxPalHandle = CK_INVALID_HANDLE;
        }

        return xResult;
    }

/**
 * @brief Helper function to load an EC group to the mbed TLS pk context.
 * @param[in] pxMbedContext mbedtls context used to load EC group params.
 *
 */
    static CK_RV prvLoadEcGroup( mbedtls_pk_context * pxMbedContext )
    {
        CK_RV xResult = CKR_OK;
        mbedtls_ecp_keypair * pxKeyPair;
        int32_t lMbedTLSResult = 0;

        pxKeyPair = mbedtls_calloc( 1, sizeof( mbedtls_ecp_keypair ) );

        if( pxKeyPair != NULL )
        {
            /* Initialize the info. */
            pxMbedContext->pk_info = mbedtls_pk_info_from_type( MBEDTLS_PK_ECKEY );

            /* Initialize the context. */
            pxMbedContext->pk_ctx = pxKeyPair;
            mbedtls_ecp_keypair_init( pxKeyPair );
            mbedtls_ecp_group_init( &pxKeyPair->grp );

            /* At this time, only P-256 curves are supported. */
            lMbedTLSResult = mbedtls_ecp_group_load( &pxKeyPair->grp,
                                                     MBEDTLS_ECP_DP_SECP256R1 );

            if( lMbedTLSResult != 0 )
            {
                LogError( ( "Failed creating an EC key. "
                            "mbedtls_ecp_group_load failed: mbed "
                            "TLS error = %s : %s.",
                            mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                            mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                xResult = CKR_FUNCTION_FAILED;
            }
        }
        else
        {
            LogError( ( "Failed creating an EC key. Could not allocate a "
                        "mbedtls_ecp_keypair struct." ) );
            xResult = CKR_HOST_MEMORY;
        }

        return xResult;
    }

/**
 * @brief Helper function for importing elliptic curve keys from
 * template using C_CreateObject.
 * @param[in] pxTemplate templates to search for a key in.
 * @param[in] ulCount length of templates array.
 * @param[in] pxObject PKCS #11 object handle.
 * @param[in] xIsPrivate boolean indicating whether the key is private or public.
 */
    static CK_RV prvCreateECKey( CK_ATTRIBUTE * pxTemplate,
                                 CK_ULONG ulCount,
                                 CK_OBJECT_HANDLE_PTR pxObject,
                                 CK_BBOOL xIsPrivate )


    {
        CK_RV xResult = CKR_OK;
        uint32_t ulIndex;
        CK_ATTRIBUTE_PTR pxLabel = NULL;
        CK_OBJECT_HANDLE xPalHandle = CK_INVALID_HANDLE;
        mbedtls_pk_context xMbedContext;

        mbedtls_pk_init( &xMbedContext );

        prvGetLabel( &pxLabel, pxTemplate, ulCount );

        if( pxLabel == NULL )
        {
            LogError( ( "Failed to create an EC key. Received a NULL label pointer." ) );
            xResult = CKR_ARGUMENTS_BAD;
        }
        else
        {
            xResult = prvGetExistingKeyComponent( &xPalHandle, &xMbedContext, pxLabel );
        }

        if( ( xResult == CKR_OK ) && ( xPalHandle == CK_INVALID_HANDLE ) )
        {
            /* An mbedTLS key is comprised of 2 pieces of data- an "info" and a "context".
             * Since a valid key was not found by prvGetExistingKeyComponent, we are going to initialize
             * the structure so that the mbedTLS structures will look the same as they would if a key
             * had been found, minus the private key component. */

            /* If a key had been found by prvGetExistingKeyComponent, the keypair context
             * would have been malloc'ed. */
            xResult = prvLoadEcGroup( &xMbedContext );
        }

        /* Key will be assembled in the mbedTLS key context and then exported to DER for storage. */

        if( xResult == CKR_OK )
        {
            for( ulIndex = 0; ulIndex < ulCount; ulIndex++ )
            {
                xResult = prvEcKeyAttParse( &( pxTemplate[ ulIndex ] ), &xMbedContext, xIsPrivate );

                if( xResult != CKR_OK )
                {
                    break;
                }
            }
        }

        if( xResult == CKR_OK )
        {
            xResult = prvSaveDerKeyToPal( &xMbedContext,
                                          pxObject,
                                          pxLabel,
                                          CKK_EC,
                                          xIsPrivate );
        }

        /* Clean up the mbedTLS key context. */
        mbedtls_pk_free( &xMbedContext );

        return xResult;
    }
#endif /* ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM */

/**
 * @brief Helper function for parsing RSA Private Key attribute templates
 * for C_CreateObject.
 * @param[in] pxTemplate templates to search for a key in.
 * @param[in] ulCount length of templates array.
 * @param[in] pxObject PKCS #11 object handle.
 * @param[in] xIsPrivate boolean indicating whether the key is private or public.
 */
static CK_RV prvCreateRsaKey( CK_ATTRIBUTE * pxTemplate,
                              CK_ULONG ulCount,
                              CK_OBJECT_HANDLE_PTR pxObject,
                              CK_BBOOL xIsPrivate )
{
    CK_RV xResult = CKR_OK;
    mbedtls_pk_context xMbedContext = { 0 };
    uint32_t ulIndex;
    CK_ATTRIBUTE_PTR pxLabel = NULL;

    /* mbedtls_rsa_context must be malloc'ed to use with mbedtls_pk_free function. */
    mbedtls_rsa_context * pxRsaCtx = mbedtls_calloc( 1, sizeof( mbedtls_rsa_context ) );

    prvGetLabel( &pxLabel, pxTemplate, ulCount );

    if( pxLabel == NULL )
    {
        LogError( ( "Failed creating an RSA key. Label was a NULL pointer." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( pxRsaCtx != NULL )
    {
        mbedtls_pk_init( &xMbedContext );
        xMbedContext.pk_ctx = pxRsaCtx;
        xMbedContext.pk_info = mbedtls_pk_info_from_type( MBEDTLS_PK_RSA );

        #if MBEDTLS_VERSION_NUMBER < 0x03000000
            mbedtls_rsa_init( pxRsaCtx, MBEDTLS_RSA_PKCS_V15, 0 /*ignored.*/ );
        #else
            mbedtls_rsa_init( pxRsaCtx );
        #endif /* MBEDTLS_VERSION_NUMBER < 0x03000000 */
    }
    else
    {
        LogError( ( "Failed creating an RSA key. Could not malloc a "
                    "mbedtls_rsa_context struct." ) );
        xResult = CKR_HOST_MEMORY;
    }

    if( xResult == CKR_OK )
    {
        /* Parse template and collect the relevant parts. */
        for( ulIndex = 0; ulIndex < ulCount; ulIndex++ )
        {
            xResult = prvRsaKeyAttParse( &( pxTemplate[ ulIndex ] ), xMbedContext.pk_ctx, xIsPrivate );

            if( xResult != CKR_OK )
            {
                break;
            }
        }
    }

    if( xResult == CKR_OK )
    {
        xResult = prvSaveDerKeyToPal( &xMbedContext,
                                      pxObject,
                                      pxLabel,
                                      CKK_RSA,
                                      xIsPrivate );
    }

    /* Clean up the mbedTLS key context. */
    mbedtls_pk_free( &xMbedContext );

    return xResult;
}

/**
 * @brief Parses attribute values for a HMAC Key.
 */
static CK_RV prvHMACKeyAttParse( const CK_ATTRIBUTE * pxAttribute,
                                 CK_BYTE_PTR * ppxHmacKey,
                                 CK_ULONG * pulHmacKeyLen )
{
    CK_RV xResult = CKR_OK;
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xBool = ( CK_BBOOL ) CK_FALSE;

    switch( pxAttribute->type )
    {
        case ( CKA_CLASS ):
        case ( CKA_KEY_TYPE ):
        case ( CKA_LABEL ):
            /* Do nothing. These values were parsed previously. */
            break;

        case ( CKA_TOKEN ):
        case ( CKA_VERIFY ):
        case ( CKA_SIGN ):

            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, sizeof( CK_BBOOL ) );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool != ( CK_BBOOL ) CK_TRUE )
            {
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            }

            break;

        case ( CKA_VALUE ):

            if( ( pxAttribute->ulValueLen >= PKCS11_SHA256_HMAC_MIN_SIZE ) &&
                ( pxAttribute->pValue != NULL ) )
            {
                *ppxHmacKey = pxAttribute->pValue;
                *pulHmacKeyLen = pxAttribute->ulValueLen;
            }
            else
            {
                LogError( ( "Failed to create SHA256-HMAC secret key. "
                            "Key should be at least 32 bytes and/or non-NULL." ) );
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            }

            break;

        default:
            xResult = CKR_ATTRIBUTE_TYPE_INVALID;
            break;
    }

    return xResult;
}

/**
 * @brief Helper function for parsing SHA256-HMAC Key attribute templates
 * for C_CreateObject.
 * @param[in] pxTemplate templates to search for a key in.
 * @param[in] ulCount length of templates array.
 * @param[in] pxObject PKCS #11 object handle.
 */
static CK_RV prvCreateSHA256HMAC( CK_ATTRIBUTE * pxTemplate,
                                  CK_ULONG ulCount,
                                  CK_OBJECT_HANDLE_PTR pxObject )
{
    CK_RV xResult = CKR_OK;
    uint32_t ulIndex;
    CK_ATTRIBUTE_PTR pxLabel = NULL;
    CK_BYTE_PTR pxSecretKeyValue = NULL;
    CK_ULONG ulSecretKeyValueLen = 0;
    CK_OBJECT_HANDLE xPalHandle = CK_INVALID_HANDLE;

    prvGetLabel( &pxLabel, pxTemplate, ulCount );

    if( pxLabel == NULL )
    {
        LogError( ( "Failed creating a SHA256-HMAC key. Label was a NULL pointer." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( xResult == CKR_OK )
    {
        for( ulIndex = 0; ulIndex < ulCount; ulIndex++ )
        {
            xResult = prvHMACKeyAttParse( &( pxTemplate[ ulIndex ] ), &pxSecretKeyValue, &ulSecretKeyValueLen );

            if( xResult != CKR_OK )
            {
                break;
            }
        }
    }

    if( ( xResult == CKR_OK ) && ( pxSecretKeyValue != NULL ) &&
        ( ulSecretKeyValueLen >= PKCS11_SHA256_HMAC_MIN_SIZE ) )
    {
        xPalHandle = PKCS11_PAL_SaveObject( pxLabel,
                                            pxSecretKeyValue,
                                            ulSecretKeyValueLen );

        if( xPalHandle == CK_INVALID_HANDLE )
        {
            LogError( ( "Failed saving HMAC secret key to flash. Failed to the PKCS #11 PAL." ) );
            xResult = CKR_DEVICE_MEMORY;
        }
        else
        {
            xResult = prvAddObjectToList( xPalHandle, pxObject, pxLabel->pValue, pxLabel->ulValueLen );
        }
    }

    return xResult;
}

/**
 * @brief Parses attribute values for a CMAC Key.
 */
static CK_RV prvCMACKeyAttParse( const CK_ATTRIBUTE * pxAttribute,
                                 CK_BYTE_PTR * ppxCmacKey,
                                 CK_ULONG * pulCmacKeyLen )
{
    CK_RV xResult = CKR_OK;
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xBool = ( CK_BBOOL ) CK_FALSE;

    switch( pxAttribute->type )
    {
        case ( CKA_CLASS ):
        case ( CKA_KEY_TYPE ):
        case ( CKA_LABEL ):
            /* Do nothing. These values were parsed previously. */
            break;

        case ( CKA_TOKEN ):
        case ( CKA_VERIFY ):
        case ( CKA_SIGN ):

            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, sizeof( CK_BBOOL ) );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool != ( CK_BBOOL ) CK_TRUE )
            {
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            }

            break;

        case ( CKA_VALUE ):

            if( ( pxAttribute->ulValueLen >= PKCS11_AES_CMAC_MIN_SIZE ) &&
                ( pxAttribute->pValue != NULL ) )
            {
                *ppxCmacKey = pxAttribute->pValue;
                *pulCmacKeyLen = pxAttribute->ulValueLen;
            }
            else
            {
                LogError( ( "Failed to create SHA256-HMAC secret key. "
                            "Key should be at least 32 bytes and/or non-NULL." ) );
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
            }

            break;

        default:
            xResult = CKR_ATTRIBUTE_TYPE_INVALID;
            break;
    }

    return xResult;
}

/**
 * @brief Helper function for parsing AES-CMAC Key attribute templates
 * for C_CreateObject.
 * @param[in] pxTemplate templates to search for a key in.
 * @param[in] ulCount length of templates array.
 * @param[in] pxObject PKCS #11 object handle.
 */
static CK_RV prvCreateAESCMAC( CK_ATTRIBUTE * pxTemplate,
                               CK_ULONG ulCount,
                               CK_OBJECT_HANDLE_PTR pxObject )
{
    CK_RV xResult = CKR_OK;
    uint32_t ulIndex;
    CK_ATTRIBUTE_PTR pxLabel = NULL;
    CK_BYTE_PTR pxSecretKeyValue = NULL;
    CK_ULONG ulSecretKeyValueLen = 0;
    CK_OBJECT_HANDLE xPalHandle = CK_INVALID_HANDLE;

    prvGetLabel( &pxLabel, pxTemplate, ulCount );

    if( pxLabel == NULL )
    {
        LogError( ( "Failed creating an AES-CMAC key. Label was a NULL pointer." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( xResult == CKR_OK )
    {
        for( ulIndex = 0; ulIndex < ulCount; ulIndex++ )
        {
            xResult = prvCMACKeyAttParse( &( pxTemplate[ ulIndex ] ), &pxSecretKeyValue, &ulSecretKeyValueLen );

            if( xResult != CKR_OK )
            {
                break;
            }
        }
    }

    if( ( xResult == CKR_OK ) && ( pxSecretKeyValue != NULL ) )
    {
        xPalHandle = PKCS11_PAL_SaveObject( pxLabel,
                                            pxSecretKeyValue,
                                            ulSecretKeyValueLen );

        if( xPalHandle == CK_INVALID_HANDLE )
        {
            LogError( ( "Failed saving CMAC secret key to flash. Failed to the PKCS #11 PAL." ) );
            xResult = CKR_DEVICE_MEMORY;
        }
        else
        {
            xResult = prvAddObjectToList( xPalHandle, pxObject, pxLabel->pValue, pxLabel->ulValueLen );
        }
    }

    return xResult;
}

/**
 * @brief Helper function for importing private keys using template
 * C_CreateObject.
 * @param[in] pxTemplate templates to search for a key in.
 * @param[in] ulCount length of templates array.
 * @param[in] pxObject PKCS #11 object handle.
 */
static CK_RV prvCreatePrivateKey( CK_ATTRIBUTE * pxTemplate,
                                  CK_ULONG ulCount,
                                  CK_OBJECT_HANDLE_PTR pxObject )
{
    CK_RV xResult = CKR_OK;
    CK_KEY_TYPE xKeyType;

    prvGetKeyType( &xKeyType, pxTemplate, ulCount );

    if( xKeyType == CKK_RSA )
    {
        xResult = prvCreateRsaKey( pxTemplate,
                                   ulCount,
                                   pxObject,
                                   /* MISRA Ref 10.5.1 [Essential type casting] */
                                   /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                                   /* coverity[misra_c_2012_rule_10_5_violation] */
                                   ( CK_BBOOL ) CK_TRUE );
    }

    #ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM
        /* CKK_EC = CKK_ECDSA. */
        else if( xKeyType == CKK_EC )
        {
            xResult = prvCreateECKey( pxTemplate,
                                      ulCount,
                                      pxObject,
                                      /* MISRA Ref 10.5.1 [Essential type casting] */
                                      /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                                      /* coverity[misra_c_2012_rule_10_5_violation] */
                                      ( CK_BBOOL ) CK_TRUE );
        }
    #endif /* pkcs11configSUPPRESS_ECDSA_MECHANISM */
    else
    {
        LogError( ( "Failed to create a key. Tried to create a key with an "
                    "invalid or unknown mechanism." ) );
        xResult = CKR_MECHANISM_INVALID;
    }

    return xResult;
}


/**
 * @brief Helper function for importing public keys using
 * C_CreateObject.
 * @param[in] pxTemplate templates to search for a key in.
 * @param[in] ulCount length of templates array.
 * @param[in] pxObject PKCS #11 object handle.
 */
static CK_RV prvCreatePublicKey( CK_ATTRIBUTE * pxTemplate,
                                 CK_ULONG ulCount,
                                 CK_OBJECT_HANDLE_PTR pxObject )
{
    CK_KEY_TYPE xKeyType = 0;
    CK_RV xResult = CKR_OK;

    prvGetKeyType( &xKeyType, pxTemplate, ulCount );

    if( xKeyType == CKK_RSA )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        xResult = prvCreateRsaKey( pxTemplate, ulCount, pxObject, ( CK_BBOOL ) CK_FALSE );
    }

    #ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM
        else if( xKeyType == CKK_EC ) /* CKK_EC = CKK_ECDSA. */
        {
            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            xResult = prvCreateECKey( pxTemplate, ulCount, pxObject, ( CK_BBOOL ) CK_FALSE );
        }
    #endif /* ifndef pkcs11configSUPPRESS_ECDSA_MECHANISM */
    else
    {
        LogError( ( "Failed to create public key. Received an invalid mechanism. "
                    "Invalid key type 0x%0lX", ( unsigned long int ) xKeyType ) );
        xResult = CKR_MECHANISM_INVALID;
    }

    return xResult;
}

/**
 * @brief Helper function for importing secret keys using template
 * C_CreateObject.
 * @param[in] pxTemplate templates to search for a key in.
 * @param[in] ulCount length of templates array.
 * @param[in] pxObject PKCS #11 object handle.
 */
static CK_RV prvCreateSecretKey( CK_ATTRIBUTE * pxTemplate,
                                 CK_ULONG ulCount,
                                 CK_OBJECT_HANDLE_PTR pxObject )
{
    CK_RV xResult = CKR_OK;
    CK_KEY_TYPE xKeyType;

    prvGetKeyType( &xKeyType, pxTemplate, ulCount );

    if( xKeyType == CKK_SHA256_HMAC )
    {
        xResult = prvCreateSHA256HMAC( pxTemplate,
                                       ulCount,
                                       pxObject );
    }
    else if( xKeyType == CKK_AES )
    {
        xResult = prvCreateAESCMAC( pxTemplate,
                                    ulCount,
                                    pxObject );
    }
    else
    {
        LogError( ( "Failed to create a key. Tried to create a key with an "
                    "invalid or unknown mechanism. Only CKK_SHA256_HMAC is "
                    "currently supported." ) );
        xResult = CKR_MECHANISM_INVALID;
    }

    return xResult;
}

/**
 * @brief Creates an object.
 *
 * @param[in] hSession                   Handle of a valid PKCS #11 session.
 * @param[in] pTemplate                  List of attributes of the object to
 *                                       be created.
 * @param[in] ulCount                    Number of attributes in pTemplate.
 * @param[out] phObject                  Pointer to the location where the created
 *                                       object's handle will be placed.
 *
 * <table>
 * <tr><th> Object Type             <th> Template Attributes
 * <tr><td rowspan="6">Certificate<td>CKA_CLASS
 * <tr>                           <td>CKA_VALUE
 * <tr>                              <td>CKA_TOKEN
 * <tr>                              <td>CKA_LABEL
 * <tr>                              <td>CKA_CERTIFICATE_TYPE
 * <tr>                              <td>CKA_VALUE
 * <tr><td rowspan="7">EC Private Key<td>CKA_CLASS
 * <tr>                              <td>CKA_KEY_TYPE
 * <tr>                              <td>CKA_TOKEN
 * <tr>                              <td>CKA_LABEL
 * <tr>                              <td>CKA_SIGN
 * <tr>                              <td>CKA_EC_PARAMS
 * <tr>                              <td>CKA_VALUE
 * <tr><td rowspan="7">EC Public Key<td>CKA_CLASS
 * <tr>                              <td>CKA_KEY_TYPE
 * <tr>                              <td>CKA_TOKEN
 * <tr>                              <td>CKA_VERIFY
 * <tr>                              <td>CKA_LABEL
 * <tr>                              <td>CKA_EC_PARAMS
 * <tr>                              <td>CKA_EC_POINT
 * <tr><td rowspan="13">RSA Private Key<td>CKA_CLASS
 * <tr>                               <td>CKA_KEY_TYPE
 * <tr>                               <td>CKA_TOKEN
 * <tr>                               <td>CKA_LABEL
 * <tr>                               <td>CKA_SIGN
 * <tr>                               <td>CKA_MODULUS
 * <tr>                               <td>CKA_PUBLIC_EXPONENT
 * <tr>                               <td>CKA_PRIME_1
 * <tr>                               <td>CKA_PRIME_2
 * <tr>                               <td>CKA_PRIVATE_EXPONENT
 * <tr>                               <td>CKA_EXPONENT_1
 * <tr>                               <td>CKA_EXPONENT_2
 * <tr>                               <td>CKA_COEFFICIENT
 * </table>
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_createobject] */
CK_DECLARE_FUNCTION( CK_RV, C_CreateObject )( CK_SESSION_HANDLE hSession,
                                              CK_ATTRIBUTE_PTR pTemplate,
                                              CK_ULONG ulCount,
                                              CK_OBJECT_HANDLE_PTR phObject )
{
    CK_OBJECT_CLASS xClass = 0;

    const P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );

    if( ( NULL == pTemplate ) ||
        ( NULL == phObject ) )
    {
        LogError( ( "Failed to create object. Received a NULL template or "
                    "object pointer." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( xResult == CKR_OK )
    {
        xResult = prvGetObjectClass( pTemplate, ulCount, &xClass );
    }

    if( xResult == CKR_OK )
    {
        LogInfo( ( "Creating a 0x%0lX type object.", ( unsigned long int ) xClass ) );

        switch( xClass )
        {
            case CKO_CERTIFICATE:
                xResult = prvCreateCertificate( pTemplate, ulCount, phObject );
                break;

            case CKO_PRIVATE_KEY:
                xResult = prvCreatePrivateKey( pTemplate, ulCount, phObject );
                break;

            case CKO_PUBLIC_KEY:
                xResult = prvCreatePublicKey( pTemplate, ulCount, phObject );
                break;

            case CKO_SECRET_KEY:
                xResult = prvCreateSecretKey( pTemplate, ulCount, phObject );
                break;

            default:
                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
                break;
        }
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_createobject] */

/**
 * @brief Destroys an object.
 *
 * @param[in] hSession                   Handle of a valid PKCS #11 session.
 * @param[in] hObject                    Handle of the object to be destroyed.
 *
 * @warning In this implementation, if either the device public key or the device
 * private key (labels pkcs11configLABEL_DEVICE_PUBLIC_KEY_FOR_TLS and
 * pkcs11configLABEL_DEVICE_PRIVATE_KEY_FOR_TLS) are deleted, both keys will
 * be destroyed.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_destroyobject] */
CK_DECLARE_FUNCTION( CK_RV, C_DestroyObject )( CK_SESSION_HANDLE hSession,
                                               CK_OBJECT_HANDLE hObject )
{
    const P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );
    CK_OBJECT_HANDLE xPalHandle = CK_INVALID_HANDLE;
    CK_BYTE_PTR pcLabel = NULL;
    CK_ULONG xLabelLength = 0;


    prvFindObjectInListByHandle( hObject, &xPalHandle, &pcLabel, &xLabelLength );

    if( xPalHandle == CK_INVALID_HANDLE )
    {
        xResult = CKR_OBJECT_HANDLE_INVALID;
    }

    if( xResult == CKR_OK )
    {
        xResult = PKCS11_PAL_DestroyObject( xPalHandle );

        if( xResult == CKR_OK )
        {
            xResult = prvDeleteObjectFromList( xPalHandle );
        }
        else
        {
            LogError( ( "Failed to destroy object. PKCS11_PAL_DestroyObject failed." ) );
        }
    }
    else
    {
        LogError( ( "Failed to destroy object. The session was invalid." ) );
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_destroyobject] */

/**
 * @brief Obtains an attribute value of an object.
 * @param[in] hSession                   Handle of a valid PKCS #11 session.
 * @param[in] hObject                    PKCS #11 object handle to be queried.
 * @param[in,out] pTemplate              Attribute template.
 *                                       pxTemplate.pValue should be set to the attribute
 *                                       to be queried. pxTemplate.ulValueLen should be
 *                                       set to the length of the buffer allocated at
 *                                       pxTemplate.pValue, and will be updated to contain
 *                                       the actual length of the data copied.
 *                                       pxTemplate.pValue should be set to point to
 *                                       a buffer to receive the attribute value data.
 *                                       If parameter length is unknown,
 *                                       pxTemplate.pValue may be set to NULL, and
 *                                       this function will set the required buffer length
 *                                       in pxTemplate.ulValueLen.
 * @param[in] ulCount                    The number of attributes in the template.
 *
 * <table>
 * <tr><th> Object Type             <th> Queryable Attributes
 * <tr><td rowspan="2">Certificate<td>CKA_CLASS
 * <tr>                           <td>CKA_VALUE
 * <tr><td rowspan="3">EC Private Key<td>CKA_CLASS
 * <tr>                              <td>CKA_KEY_TYPE
 * <tr>                              <td>CKA_EC_PARAMS
 * <tr><td rowspan="4">EC Public Key<td>CKA_CLASS
 * <tr>                             <td>CKA_KEY_TYPE
 * <tr>                             <td>CKA_EC_PARAMS
 * <tr>                             <td>CKA_EC_POINT
 * <tr><td rowspan="2">RSA Private Key<td>CKA_CLASS
 * <tr>                               <td>CKA_KEY_TYPE
 * <tr><td rowspan="2">RSA Public Key<td>CKA_CLASS
 * <tr>                               <td>CKA_KEY_TYPE
 * </table>
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_getattributevalue] */
CK_DECLARE_FUNCTION( CK_RV, C_GetAttributeValue )( CK_SESSION_HANDLE hSession,
                                                   CK_OBJECT_HANDLE hObject,
                                                   CK_ATTRIBUTE_PTR pTemplate,
                                                   CK_ULONG ulCount )
{
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xIsPrivate = ( CK_BBOOL ) CK_TRUE;
    CK_ULONG iAttrib;
    mbedtls_pk_context xKeyContext = { 0 };
    mbedtls_x509_crt xMbedX509Context = { 0 };
    mbedtls_pk_type_t xKeyType;
    const mbedtls_ecp_keypair * pxKeyPair;
    const mbedtls_rsa_context * pxRsaContext;
    CK_KEY_TYPE xPkcsKeyType = ( CK_KEY_TYPE ) ~0UL;
    CK_OBJECT_CLASS xClass = ~0UL;
    CK_BYTE_PTR pxObjectValue = NULL;
    CK_ULONG ulLength = 0;
    const CK_BYTE ucP256Oid[] = pkcs11DER_ENCODED_OID_P256;
    int32_t lMbedTLSResult = 0;
    CK_OBJECT_HANDLE xPalHandle = CK_INVALID_HANDLE;
    CK_ULONG xSize = 0;
    size_t xMbedSize = 0;
    CK_BYTE_PTR pcLabel = NULL;

    const P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );

    if( ( CKR_OK == xResult ) && ( ( ( NULL == pTemplate ) ) || ( 0UL == ulCount ) ) )
    {
        LogError( ( "Failed to get attribute. The template was a NULL pointer "
                    "or the attribute count was 0." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( ( CKR_OK == xResult ) && ( CK_INVALID_HANDLE == hObject ) )
    {
        LogError( ( "Failed to get attribute. The object handle was invalid." ) );
        xResult = CKR_OBJECT_HANDLE_INVALID;
    }

    if( xResult == CKR_OK )
    {
        /*
         * Copy the object into a buffer.
         */
        prvFindObjectInListByHandle( hObject, &xPalHandle, &pcLabel, &xSize ); /*pcLabel and xSize are ignored. */

        if( xPalHandle != CK_INVALID_HANDLE )
        {
            xResult = PKCS11_PAL_GetObjectValue( xPalHandle, &pxObjectValue, &ulLength, &xIsPrivate );
        }
        else
        {
            LogError( ( "Failed to get attribute. Could not find a valid "
                        "PKCS #11 PAL handle." ) );
            xResult = CKR_OBJECT_HANDLE_INVALID;
        }
    }

    /* Determine what kind of object we are dealing with. */
    if( xResult == CKR_OK )
    {
        /* Initialize mbed TLS key context. */
        mbedtls_pk_init( &xKeyContext );

        /* Initialize mbed TLS x509 context. */
        mbedtls_x509_crt_init( &xMbedX509Context );

        #if MBEDTLS_VERSION_NUMBER < 0x03000000
            lMbedTLSResult = mbedtls_pk_parse_key( &xKeyContext,
                                                   pxObjectValue, ulLength,
                                                   NULL, 0 );
        #else
            lMbedTLSResult = mbedtls_pk_parse_key( &xKeyContext,
                                                   pxObjectValue, ulLength,
                                                   NULL, 0,
                                                   mbedtls_ctr_drbg_random, &xP11Context.xMbedDrbgCtx );
        #endif /* MBEDTLS_VERSION_NUMBER < 0x03000000 */

        if( lMbedTLSResult == 0 )
        {
            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xIsPrivate == ( CK_BBOOL ) CK_TRUE )
            {
                LogDebug( ( "Object was a private key." ) );
                xClass = CKO_PRIVATE_KEY;
            }
            else
            {
                LogDebug( ( "Object was a public key." ) );
                xClass = CKO_PUBLIC_KEY;
            }
        }
        else if( 0 == mbedtls_pk_parse_public_key( &xKeyContext, pxObjectValue, ulLength ) )
        {
            LogDebug( ( "Object was a public key." ) );
            xClass = CKO_PUBLIC_KEY;
        }
        else if( 0 == mbedtls_x509_crt_parse( &xMbedX509Context, pxObjectValue, ulLength ) )
        {
            LogDebug( ( "Object was a certificate." ) );
            xClass = CKO_CERTIFICATE;
        }
        else
        {
            LogDebug( ( "Object handle was invalid." ) );
            xResult = CKR_OBJECT_HANDLE_INVALID;
        }
    }

    if( xResult == CKR_OK )
    {
        for( iAttrib = 0; iAttrib < ulCount; iAttrib++ )
        {
            if( ( xResult != CKR_ATTRIBUTE_SENSITIVE ) && ( xResult != CKR_ATTRIBUTE_TYPE_INVALID ) && ( xResult != CKR_BUFFER_TOO_SMALL ) && ( xResult != CKR_OK ) )
            {
                break;
            }

            switch( pTemplate[ iAttrib ].type )
            {
                case CKA_CLASS:

                    if( pTemplate[ iAttrib ].pValue == NULL )
                    {
                        pTemplate[ iAttrib ].ulValueLen = sizeof( CK_OBJECT_CLASS );
                    }
                    else
                    {
                        if( pTemplate[ iAttrib ].ulValueLen == sizeof( CK_OBJECT_CLASS ) )
                        {
                            ( void ) memcpy( pTemplate[ iAttrib ].pValue, &xClass, sizeof( CK_OBJECT_CLASS ) );
                        }
                        else
                        {
                            LogError( ( "Failed to parse attribute template. "
                                        "Received a buffer smaller than CK_OBJECT_CLASS." ) );
                            xResult = CKR_BUFFER_TOO_SMALL;
                            pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                        }
                    }

                    break;

                case CKA_PUBLIC_KEY_INFO:
                case CKA_VALUE:

                    /* MISRA Ref 10.5.1 [Essential type casting] */
                    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                    /* coverity[misra_c_2012_rule_10_5_violation] */
                    if( xIsPrivate == ( CK_BBOOL ) CK_TRUE )
                    {
                        pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                        LogError( ( "Failed to parse attribute. This data is "
                                    "sensitive and the value will not be returned." ) );
                        xResult = CKR_ATTRIBUTE_SENSITIVE;
                        pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                    }
                    else
                    {
                        if( pTemplate[ iAttrib ].pValue == NULL )
                        {
                            pTemplate[ iAttrib ].ulValueLen = ulLength;
                        }
                        else if( pTemplate[ iAttrib ].ulValueLen < ulLength )
                        {
                            LogError( ( "Failed to parse attribute. Buffer was "
                                        "too small to contain data. Expected %lu "
                                        "but got %lu.",
                                        ( unsigned long int ) ulLength,
                                        ( unsigned long int ) pTemplate[ iAttrib ].ulValueLen ) );
                            xResult = CKR_BUFFER_TOO_SMALL;
                            pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                        }
                        else
                        {
                            ( void ) memcpy( pTemplate[ iAttrib ].pValue, pxObjectValue, ulLength );
                        }
                    }

                    break;

                case CKA_KEY_TYPE:

                    if( pTemplate[ iAttrib ].pValue == NULL )
                    {
                        pTemplate[ iAttrib ].ulValueLen = sizeof( CK_KEY_TYPE );
                    }
                    else if( pTemplate[ iAttrib ].ulValueLen < sizeof( CK_KEY_TYPE ) )
                    {
                        LogError( ( "Failed to parse attribute. Expected buffer "
                                    "of size CK_KEY_TYPE." ) );
                        xResult = CKR_BUFFER_TOO_SMALL;
                        pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                    }
                    else
                    {
                        xKeyType = mbedtls_pk_get_type( &xKeyContext );

                        switch( xKeyType )
                        {
                            case MBEDTLS_PK_RSA:
                            case MBEDTLS_PK_RSA_ALT:
                            case MBEDTLS_PK_RSASSA_PSS:
                                xPkcsKeyType = CKK_RSA;
                                break;

                            case MBEDTLS_PK_ECKEY:
                            case MBEDTLS_PK_ECKEY_DH:
                                xPkcsKeyType = CKK_EC;
                                break;

                            case MBEDTLS_PK_ECDSA:
                                xPkcsKeyType = CKK_ECDSA;
                                break;

                            default:
                                LogError( ( "Failed to parse attribute. "
                                            "Could not parse key type." ) );
                                xResult = CKR_ATTRIBUTE_VALUE_INVALID;
                                pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                                break;
                        }

                        ( void ) memcpy( pTemplate[ iAttrib ].pValue, &xPkcsKeyType, sizeof( CK_KEY_TYPE ) );
                    }

                    break;

                case CKA_EC_PARAMS:

                    if( pTemplate[ iAttrib ].pValue == NULL )
                    {
                        pTemplate[ iAttrib ].ulValueLen = sizeof( ucP256Oid );
                    }
                    else
                    {
                        if( pTemplate[ iAttrib ].ulValueLen < sizeof( ucP256Oid ) )
                        {
                            LogError( ( "Failed to parse attribute. "
                                        "CKA_EC_PARAMS buffer too small." ) );
                            xResult = CKR_BUFFER_TOO_SMALL;
                            pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                        }
                        else
                        {
                            ( void ) memcpy( pTemplate[ iAttrib ].pValue, ucP256Oid, sizeof( ucP256Oid ) );
                        }
                    }

                    break;

                case CKA_EC_POINT:

                    if( pTemplate[ iAttrib ].pValue == NULL )
                    {
                        pTemplate[ iAttrib ].ulValueLen = pkcs11EC_POINT_LENGTH;
                    }
                    else
                    {
                        if( pTemplate[ iAttrib ].ulValueLen == pkcs11EC_POINT_LENGTH )
                        {
                            pxKeyPair = ( mbedtls_ecp_keypair * ) xKeyContext.pk_ctx;
                            *( ( uint8_t * ) pTemplate[ iAttrib ].pValue ) = 0x04; /* Mark the point as uncompressed. */

                            /* Copy xSize value to avoid casting a CK_ULONG size pointer
                             * to a size_t sized pointer. */
                            xMbedSize = xSize;
                            lMbedTLSResult = mbedtls_ecp_tls_write_point( &pxKeyPair->grp,
                                                                          &pxKeyPair->Q,
                                                                          MBEDTLS_ECP_PF_UNCOMPRESSED,
                                                                          &xMbedSize,
                                                                          ( uint8_t * ) pTemplate[ iAttrib ].pValue + 1,
                                                                          pTemplate[ iAttrib ].ulValueLen - 1UL );
                            xSize = xMbedSize;

                            if( lMbedTLSResult < 0 )
                            {
                                if( lMbedTLSResult == MBEDTLS_ERR_ECP_BUFFER_TOO_SMALL )
                                {
                                    LogError( ( "Failed to extract EC point. "
                                                "CKA_EC_POINT buffer was too small." ) );
                                    xResult = CKR_BUFFER_TOO_SMALL;
                                    pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                                }
                                else
                                {
                                    LogError( ( "Failed to extract EC point. "
                                                "mbedtls_ecp_tls_write_point failed: "
                                                "mbed TLS error = %s : %s.",
                                                mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                                mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                                    xResult = CKR_FUNCTION_FAILED;
                                    pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                                }
                            }
                            else
                            {
                                pTemplate[ iAttrib ].ulValueLen = xSize + 1UL;
                            }
                        }
                        else
                        {
                            xResult = CKR_BUFFER_TOO_SMALL;
                            pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                        }
                    }

                    break;

                case CKA_MODULUS:
                case CKA_PUBLIC_EXPONENT:
                case CKA_PRIME_1:
                case CKA_PRIME_2:
                case CKA_PRIVATE_EXPONENT:
                case CKA_EXPONENT_1:
                case CKA_EXPONENT_2:
                case CKA_COEFFICIENT:

                    if( pTemplate[ iAttrib ].pValue == NULL )
                    {
                        pTemplate[ iAttrib ].ulValueLen = sizeof( mbedtls_mpi );
                    }
                    else
                    {
                        if( pTemplate[ iAttrib ].ulValueLen == sizeof( mbedtls_mpi ) )
                        {
                            pxRsaContext = ( mbedtls_rsa_context * ) xKeyContext.pk_ctx;

                            if( pxRsaContext != NULL )
                            {
                                xResult = prvGetAttributesFromRsaContext( &( pTemplate[ iAttrib ] ),
                                                                          pxRsaContext );
                            }
                            else
                            {
                                xResult = CKR_FUNCTION_FAILED;
                                pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                            }
                        }
                        else
                        {
                            xResult = CKR_BUFFER_TOO_SMALL;
                        }
                    }

                    break;

                default:
                    LogError( ( "Failed to parse attribute. Received unknown "
                                "attribute type." ) );
                    xResult = CKR_ATTRIBUTE_TYPE_INVALID;
                    pTemplate[ iAttrib ].ulValueLen = CK_UNAVAILABLE_INFORMATION;
                    break;
            }
        }

        /* Free the buffer where object was stored. */
        PKCS11_PAL_GetObjectValueCleanup( pxObjectValue, ulLength );

        /* Free the mbedTLS structure used to parse the key. */
        mbedtls_pk_free( &xKeyContext );

        /* Free the mbedTLS structure used to parse the certificate. */
        mbedtls_x509_crt_free( &xMbedX509Context );
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_getattributevalue] */

/**
 * @brief Initializes an object search operation.
 *
 * \sa C_FindObjects() and C_FindObjectsFinal() which must be called
 * after C_FindObjectsInit().
 *
 * \note FindObjects parameters are shared by a session.  Calling
 * C_FindObjectsInit(), C_FindObjects(), and C_FindObjectsFinal() with the
 * same session across different tasks may lead to unexpected results.
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 * @param[in] pTemplate                     Pointer to a template which specifies
 *                                          the object attributes to match.
 *                                          In this port, the only searchable attribute
 *                                          is object label.  All other attributes will
 *                                          be ignored.
 * @param[in] ulCount                       The number of attributes in pTemplate.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_findobjectsinit] */
CK_DECLARE_FUNCTION( CK_RV, C_FindObjectsInit )( CK_SESSION_HANDLE hSession,
                                                 CK_ATTRIBUTE_PTR pTemplate,
                                                 CK_ULONG ulCount )
{
    P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );
    CK_BYTE * pxFindObjectLabel = NULL;
    uint32_t ulIndex;
    CK_ATTRIBUTE xAttribute;

    if( NULL == pTemplate )
    {
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( ( ulCount < 1UL ) || ( ulCount > 2UL ) )
    {
        xResult = CKR_ARGUMENTS_BAD;
        LogError( ( "Failed to initialize find object operation. Find objects "
                    "does not support searching by %lu attributes. Expected to "
                    "search with either 1 or 2 attributes.", ( unsigned long int ) ulCount ) );
    }

    if( xResult == CKR_OK )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        if( prvOperationActive( pxSession ) == ( CK_BBOOL ) CK_TRUE )
        {
            xResult = CKR_OPERATION_ACTIVE;
            LogError( ( "Failed to initialize find object operation. Find "
                        "object operation was already in progress." ) );
        }
    }

    /* Search template for label.
     * NOTE: This port only supports looking up objects by CKA_LABEL and all
     * other search attributes are ignored. */
    if( xResult == CKR_OK )
    {
        xResult = CKR_TEMPLATE_INCOMPLETE;

        for( ulIndex = 0; ulIndex < ulCount; ulIndex++ )
        {
            xAttribute = pTemplate[ ulIndex ];

            if( ( xAttribute.type == CKA_LABEL ) && ( xAttribute.ulValueLen <= pkcs11configMAX_LABEL_LENGTH ) )
            {
                /* Plus one to leave room for a NULL terminator. */
                pxFindObjectLabel = mbedtls_calloc( 1, xAttribute.ulValueLen + 1UL );

                if( pxFindObjectLabel != NULL )
                {
                    pxSession->xFindObjectLabelLen = xAttribute.ulValueLen;
                    pxSession->pxFindObjectLabel = pxFindObjectLabel;
                    ( void ) memcpy( pxSession->pxFindObjectLabel, xAttribute.pValue, xAttribute.ulValueLen );
                    xResult = CKR_OK;
                }
                else
                {
                    LogError( ( "Failed to initialize find object operation. Failed to "
                                "allocate %lu bytes.", ( unsigned long int ) xAttribute.ulValueLen + 1UL ) );
                    xResult = CKR_HOST_MEMORY;
                }
            }
            else
            {
                LogDebug( ( "Search parameters other than label are ignored." ) );
            }
        }
    }

    /* Clean up memory if there was an error parsing the template. */
    if( ( pxSession != NULL ) && ( xResult != CKR_OK ) && ( xResult != CKR_OPERATION_ACTIVE ) )
    {
        mbedtls_free( pxFindObjectLabel );
        pxSession->pxFindObjectLabel = NULL;
        pxSession->xFindObjectLabelLen = 0;
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_findobjectsinit] */

/**
 * @brief Initializes an object search operation.
 *
 * \sa C_FindObjectsInit() which must be called before calling C_FindObjects()
 * and C_FindObjectsFinal(), which must be called after.
 *
 * \note FindObjects parameters are shared by a session.  Calling
 * C_FindObjectsInit(), C_FindObjects(), and C_FindObjectsFinal() with the
 * same session across different tasks may lead to unexpected results.
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 * @param[out] phObject                     Points to the handle of the object to
 *                                          be found.
 * @param[in] ulMaxObjectCount              The size of the phObject object handle
 *                                          array. In this port, this value should
 *                                          always be set to 1, as searching for
 *                                          multiple objects is not supported.
 * @param[out] pulObjectCount               The actual number of objects that are
 *                                          found. In this port, if an object is found
 *                                          this value will be 1, otherwise if the
 *                                          object is not found, it will be set to 0.
 *
 * \note In the event that an object does not exist, CKR_OK will be returned, but
 * pulObjectCount will be set to 0.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_findobjects] */
CK_DECLARE_FUNCTION( CK_RV, C_FindObjects )( CK_SESSION_HANDLE hSession,
                                             CK_OBJECT_HANDLE_PTR phObject,
                                             CK_ULONG ulMaxObjectCount,
                                             CK_ULONG_PTR pulObjectCount )
{
    P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );

    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_OBJECT_HANDLE xPalHandle = CK_INVALID_HANDLE;

    /*
     * Check parameters.
     */
    if( ( NULL == phObject ) ||
        ( NULL == pulObjectCount ) )
    {
        LogError( ( "Failed to find objects. The object handle or the object "
                    "count pointer was NULL." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( xResult == CKR_OK )
    {
        if( pxSession->pxFindObjectLabel == NULL )
        {
            LogError( ( "Failed to find objects. The PKCS #11 stack must be "
                        "initialized before any operations." ) );
            xResult = CKR_OPERATION_NOT_INITIALIZED;
        }

        if( 1u != ulMaxObjectCount )
        {
            xResult = CKR_ARGUMENTS_BAD;
            LogError( ( "Failed to find objects. Searching for anything other "
                        "than 1 object at a time is not supported." ) );
        }
    }

    if( xResult == CKR_OK )
    {
        /* Try to find the object in module's list first. */
        prvFindObjectInListByLabel( pxSession->pxFindObjectLabel, pxSession->xFindObjectLabelLen, &xPalHandle, phObject );

        /* Check with the PAL if the object was previously stored. */
        if( *phObject == CK_INVALID_HANDLE )
        {
            LogDebug( ( "Could not find the object handle in the list. "
                        "Trying to search PKCS #11 PAL for object." ) );
            xPalHandle = PKCS11_PAL_FindObject( pxSession->pxFindObjectLabel, pxSession->xFindObjectLabelLen );
        }

        if( xPalHandle != CK_INVALID_HANDLE )
        {
            LogDebug( ( "Found object in PAL. Adding object handle to list." ) );
            xResult = prvAddObjectToList( xPalHandle, phObject, pxSession->pxFindObjectLabel, pxSession->xFindObjectLabelLen );
            *pulObjectCount = 1;
        }
        else
        {
            /* Note: Objects living in header files are not destroyed. */
            /* According to the PKCS #11 standard, not finding an object results in a CKR_OK return value with an object count of 0. */
            *pulObjectCount = 0;
        }
    }

    /* Clean up memory if there was an error finding the object. */
    if( xResult != CKR_OK )
    {
        if( pxSession != NULL )
        {
            mbedtls_free( pxSession->pxFindObjectLabel );
            pxSession->pxFindObjectLabel = NULL;
            pxSession->xFindObjectLabelLen = 0;
        }
    }

    return xResult;
}

/* @[declare_pkcs11_mbedtls_c_findobjects] */

/**
 * @brief Finishes an object search operation.
 *
 * \sa C_FindObjectsInit(), C_FindObjects() which must be called before
 * calling C_FindObjectsFinal().
 *
 * \note FindObjects parameters are shared by a session.  Calling
 * C_FindObjectsInit(), C_FindObjects(), and C_FindObjectsFinal() with the
 * same session across different tasks may lead to unexpected results.
 *
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_findobjectsfinal] */
CK_DECLARE_FUNCTION( CK_RV, C_FindObjectsFinal )( CK_SESSION_HANDLE hSession )
{
    P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );

    /*
     * Check parameters.
     */
    if( xResult == CKR_OK )
    {
        if( pxSession->pxFindObjectLabel == NULL )
        {
            LogError( ( "Failed to end find objects operation. Find operation "
                        "must be initialized." ) );
            xResult = CKR_OPERATION_NOT_INITIALIZED;
        }
    }

    if( xResult == CKR_OK )
    {
        /*
         * Clean-up find objects state.
         */
        mbedtls_free( pxSession->pxFindObjectLabel );
        pxSession->pxFindObjectLabel = NULL;
        pxSession->xFindObjectLabelLen = 0;
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_findobjectsfinal] */

/**
 * @brief Initializes a message-digesting operation.
 *
 * \sa C_DigestUpdate(), C_DigestFinal()
 *
 * \note Digest parameters are shared by a session.  Calling
 * C_DigestInit(), C_DigestUpdate(), and C_DigestFinal() with the
 * same session across different tasks may lead to unexpected results.
 *
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 * @param[in] pMechanism                    Digesting mechanism.  This port only supports
 *                                          the mechanism CKM_SHA256.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_digestinit] */
CK_DECLARE_FUNCTION( CK_RV, C_DigestInit )( CK_SESSION_HANDLE hSession,
                                            CK_MECHANISM_PTR pMechanism )
{
    P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );
    int32_t lMbedTLSResult = 0;

    if( pMechanism == NULL )
    {
        LogError( ( "Failed to initialize digest operation. Mechanism pointer "
                    "was NULL." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( xResult == CKR_OK )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        if( prvOperationActive( pxSession ) == ( CK_BBOOL ) CK_TRUE )
        {
            LogError( ( "Failed to initialize digest operation. An operation "
                        "was already active." ) );
            xResult = CKR_OPERATION_ACTIVE;
        }
    }

    if( xResult == CKR_OK )
    {
        if( pMechanism->mechanism != CKM_SHA256 )
        {
            LogError( ( "Failed to initialize digest operation. Currently only "
                        "the CKM_SHA256 mechanism is supported." ) );
            xResult = CKR_MECHANISM_INVALID;
        }
    }

    /*
     * Initialize the requested hash type
     */
    if( xResult == CKR_OK )
    {
        mbedtls_sha256_init( &pxSession->xSHA256Context );
        #if MBEDTLS_VERSION_NUMBER < 0x03000000
            lMbedTLSResult = mbedtls_sha256_starts_ret( &pxSession->xSHA256Context, 0 );
        #else
            lMbedTLSResult = mbedtls_sha256_starts( &pxSession->xSHA256Context, 0 );
        #endif /* MBEDTLS_VERSION_NUMBER < 0x03000000 */

        if( 0 != lMbedTLSResult )
        {
            LogError( ( "Failed to initialize digest operation. "
                        "mbedtls_sha256_starts_ret failed with: mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            xResult = CKR_FUNCTION_FAILED;
        }
        else
        {
            pxSession->xOperationDigestMechanism = pMechanism->mechanism;
        }
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_digestinit] */


/**
 * @brief Continues a multiple-part digesting operation.
 *
 * \sa C_DigestInit(), C_DigestFinal()
 *
 * \note Digest parameters are shared by a session.  Calling
 * C_DigestInit(), C_DigestUpdate(), and C_DigestFinal() with the
 * same session across different tasks may lead to unexpected results.
 *
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 * @param[in] pPart                         Pointer to the data to be added to the digest.
 * @param[in] ulPartLen                     Length of the data located at pPart.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_digestupdate] */
CK_DECLARE_FUNCTION( CK_RV, C_DigestUpdate )( CK_SESSION_HANDLE hSession,
                                              CK_BYTE_PTR pPart,
                                              CK_ULONG ulPartLen )
{
    P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );
    int32_t lMbedTLSResult = 0;

    if( pPart == NULL )
    {
        LogError( ( "Failed to start digest operation. Received a NULL pointer "
                    "in digest request." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( xResult == CKR_OK )
    {
        if( pxSession->xOperationDigestMechanism != CKM_SHA256 )
        {
            LogError( ( "Failed to start digest operation. CKM_SHA256 is the "
                        "expected digest mechanism." ) );
            xResult = CKR_OPERATION_NOT_INITIALIZED;
        }
    }

    if( xResult == CKR_OK )
    {
        #if MBEDTLS_VERSION_NUMBER < 0x03000000
            lMbedTLSResult = mbedtls_sha256_update_ret( &pxSession->xSHA256Context, pPart, ulPartLen );
        #else
            lMbedTLSResult = mbedtls_sha256_update( &pxSession->xSHA256Context, pPart, ulPartLen );
        #endif /* MBEDTLS_VERSION_NUMBER < 0x03000000 */

        if( 0 != lMbedTLSResult )
        {
            LogError( ( "Failed to perform digest operation. "
                        "mbedtls_sha256_update_ret failed: mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            pxSession->xOperationDigestMechanism = pkcs11NO_OPERATION;
            xResult = CKR_FUNCTION_FAILED;
        }
    }

    if( ( xResult != CKR_OK ) && ( xResult != CKR_SESSION_HANDLE_INVALID ) &&
        ( xResult != CKR_OPERATION_NOT_INITIALIZED ) )
    {
        LogDebug( ( "Tearing down operation due to errors." ) );
        pxSession->xOperationDigestMechanism = pkcs11NO_OPERATION;
        mbedtls_sha256_free( &pxSession->xSHA256Context );
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_digestupdate] */

/**
 * @brief Finishes a multiple-part digesting operation.
 *
 * \sa C_DigestInit(), C_DigestUpdate()
 *
 * \note Digest parameters are shared by a session.  Calling
 * C_DigestInit(), C_DigestUpdate(), and C_DigestFinal() with the
 * same session across different tasks may lead to unexpected results.
 *
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 * @param[out] pDigest                      Pointer to the location that receives
 *                                          the message digest.  Memory must be allocated
 *                                          by the caller.                                          Caller is responsible for allocating memory.
 *                                          Providing NULL for this input will cause
 *                                          pulDigestLen to be updated for length of
 *                                          buffer required.
 * @param[in,out] pulDigestLen              Points to the location that holds the length
 *                                          of the message digest.  If pDigest is NULL,
 *                                          this value is updated to contain the length
 *                                          of the buffer needed to hold the digest. Else
 *                                          it is updated to contain the actual length of
 *                                          the digest placed in pDigest.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_digestfinal] */
CK_DECLARE_FUNCTION( CK_RV, C_DigestFinal )( CK_SESSION_HANDLE hSession,
                                             CK_BYTE_PTR pDigest,
                                             CK_ULONG_PTR pulDigestLen )
{
    P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );
    int32_t lMbedTLSResult = 0;


    if( pulDigestLen == NULL )
    {
        LogError( ( "Failed to finish digest operation. Digest Length pointer "
                    "was NULL." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( xResult == CKR_OK )
    {
        if( pxSession->xOperationDigestMechanism != CKM_SHA256 )
        {
            LogError( ( "Failed to finish digest operation. Digest operation "
                        "was not initialized." ) );
            xResult = CKR_OPERATION_NOT_INITIALIZED;
            pxSession->xOperationDigestMechanism = pkcs11NO_OPERATION;
        }
    }

    if( xResult == CKR_OK )
    {
        if( pDigest == NULL )
        {
            /* Supply the required buffer size. */
            *pulDigestLen = ( CK_ULONG ) pkcs11SHA256_DIGEST_LENGTH;
        }
        else
        {
            if( *pulDigestLen == ( CK_ULONG ) pkcs11SHA256_DIGEST_LENGTH )
            {
                #if MBEDTLS_VERSION_NUMBER < 0x03000000
                    lMbedTLSResult = mbedtls_sha256_finish_ret( &pxSession->xSHA256Context, pDigest );
                #else
                    lMbedTLSResult = mbedtls_sha256_finish( &pxSession->xSHA256Context, pDigest );
                #endif /* MBEDTLS_VERSION_NUMBER < 0x03000000 */

                if( 0 != lMbedTLSResult )
                {
                    LogError( ( "Failed to finish digest operation. "
                                "mbedtls_sha256_finish_ret failed: mbed TLS "
                                "error = %s : %s.",
                                mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                    xResult = CKR_FUNCTION_FAILED;
                }

                pxSession->xOperationDigestMechanism = pkcs11NO_OPERATION;
            }
            else
            {
                LogError( ( "Failed to finish digest operation. Received a "
                            "buffer that was an unexpected size. Expected %lu and "
                            "received %lu.",
                            ( unsigned long int ) pkcs11SHA256_DIGEST_LENGTH,
                            ( unsigned long int ) *pulDigestLen ) );
                xResult = CKR_BUFFER_TOO_SMALL;
            }
        }
    }

    if( ( xResult != CKR_OK ) && ( xResult != CKR_BUFFER_TOO_SMALL ) &&
        ( xResult != CKR_SESSION_HANDLE_INVALID ) &&
        ( xResult != CKR_OPERATION_NOT_INITIALIZED ) )
    {
        LogDebug( ( "Error occurred, tearing down digest operation." ) );
        pxSession->xOperationDigestMechanism = pkcs11NO_OPERATION;
        mbedtls_sha256_free( &pxSession->xSHA256Context );
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_digestfinal] */

/**
 * @brief Helper function for cleaning up a HMAC operation.
 * @param[in] pxSession   Pointer to a valid PKCS #11 session.
 */
static void prvHMACCleanUp( P11Session_t * pxSession )
{
    pxSession->xHMACKeyHandle = CK_INVALID_HANDLE;
    mbedtls_md_free( &pxSession->xHMACSecretContext );
}

/**
 * @brief Helper function for initializing a SHA256-HMAC operation.
 * @param[in] pxSession         Pointer to a valid PKCS #11 session.
 * @param[in] hKey              HMAC secret key handle.
 * @param[in] pucKeyData        HMAC secret key data.
 * @param[in] ulKeyDataLength   HMAC key Size.
 */
static CK_RV prvInitSHA256HMAC( P11Session_t * pxSession,
                                CK_OBJECT_HANDLE hKey,
                                CK_BYTE_PTR pucKeyData,
                                CK_ULONG ulKeyDataLength )
{
    CK_RV xResult = CKR_OK;
    int32_t lMbedTLSResult = 0;
    const mbedtls_md_info_t * pxMdInfo = NULL;

    mbedtls_md_init( &pxSession->xHMACSecretContext );
    pxMdInfo = mbedtls_md_info_from_type( MBEDTLS_MD_SHA256 );

    if( pxMdInfo == NULL )
    {
        LogError( ( "Failed to initialize SHA256HMAC operation. "
                    "mbedtls_md_info_from_type failed. Consider "
                    "double checking the mbedtls_md_type_t object "
                    "that was used." ) );
        xResult = CKR_FUNCTION_FAILED;
        prvHMACCleanUp( pxSession );
    }

    if( xResult == CKR_OK )
    {
        lMbedTLSResult = mbedtls_md_setup( &pxSession->xHMACSecretContext,
                                           pxMdInfo,
                                           PKCS11_USING_HMAC );

        if( lMbedTLSResult != 0 )
        {
            LogError( ( "Failed to initialize SHA256HMAC operation. "
                        "mbedtls_md_setup failed: mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            prvHMACCleanUp( pxSession );
            xResult = CKR_KEY_HANDLE_INVALID;
        }
    }

    if( xResult == CKR_OK )
    {
        lMbedTLSResult = mbedtls_md_hmac_starts( &pxSession->xHMACSecretContext,
                                                 pucKeyData, ulKeyDataLength );

        if( lMbedTLSResult != 0 )
        {
            LogError( ( "Failed to initialize SHA256HMAC operation. "
                        "mbedtls_md_setup failed: mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            prvHMACCleanUp( pxSession );
            xResult = CKR_KEY_HANDLE_INVALID;
        }
    }

    if( xResult == CKR_OK )
    {
        pxSession->xHMACKeyHandle = hKey;
    }

    return xResult;
}

/**
 * @brief Helper function for initializing a sign operation for SHA256-HMAC.
 * @param[in] pxSession         Pointer to a valid PKCS #11 session.
 * @param[in] hKey              HMAC secret key handle.
 * @param[in] pucKeyData        HMAC secret key data.
 * @param[in] ulKeyDataLength   HMAC key Size.
 */
static CK_RV prvSignInitSHA256HMAC( P11Session_t * pxSession,
                                    CK_OBJECT_HANDLE hKey,
                                    CK_BYTE_PTR pucKeyData,
                                    CK_ULONG ulKeyDataLength )
{
    CK_RV xResult = CKR_OK;

    xResult = prvInitSHA256HMAC( pxSession,
                                 hKey,
                                 pucKeyData,
                                 ulKeyDataLength );

    return xResult;
}

/**
 * @brief Helper function for cleaning up an CMAC operation.
 * @param[in] pxSession  Pointer to a valid PKCS #11 session.
 */
static void prvCMACCleanUp( P11Session_t * pxSession )
{
    pxSession->xCMACKeyHandle = CK_INVALID_HANDLE;
    mbedtls_cipher_free( &pxSession->xCMACSecretContext );
}

/**
 * @brief Helper function for initializing a AES-CMAC operation.
 * @param[in] pxSession         Pointer to a valid PKCS #11 session.
 * @param[in] hKey              CMAC secret key handle.
 * @param[in] pucKeyData        CMAC secret key data.
 * @param[in] ulKeyDataLength   CMAC key Size.
 */
static CK_RV prvInitAESCMAC( P11Session_t * pxSession,
                             CK_OBJECT_HANDLE hKey,
                             CK_BYTE_PTR pucKeyData,
                             CK_ULONG ulKeyDataLength )
{
    CK_RV xResult = CKR_OK;
    int32_t lMbedTLSResult = -1;
    const mbedtls_cipher_info_t * pxCipherInfo = NULL;
    size_t ulKeyDataBitLength = 8UL * ulKeyDataLength;

    mbedtls_cipher_init( &pxSession->xCMACSecretContext );
    pxCipherInfo = mbedtls_cipher_info_from_type( MBEDTLS_CIPHER_AES_128_ECB );

    if( pxCipherInfo == NULL )
    {
        LogError( ( "Failed to initialize AES-CMAC operation. "
                    "mbedtls_cipher_info_from_type failed. Consider "
                    "double checking the mbedtls_md_type_t object "
                    "that was used." ) );
        xResult = CKR_FUNCTION_FAILED;
        prvCMACCleanUp( pxSession );
    }

    if( xResult == CKR_OK )
    {
        lMbedTLSResult = mbedtls_cipher_setup( &pxSession->xCMACSecretContext,
                                               pxCipherInfo );

        if( lMbedTLSResult != 0 )
        {
            LogError( ( "Failed to initialize AES-CMAC operation. "
                        "mbedtls_cipher_setup failed: mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            prvCMACCleanUp( pxSession );
            xResult = CKR_KEY_HANDLE_INVALID;
        }
    }

    if( xResult == CKR_OK )
    {
        lMbedTLSResult = mbedtls_cipher_cmac_starts( &pxSession->xCMACSecretContext,
                                                     pucKeyData, ulKeyDataBitLength );

        if( lMbedTLSResult != 0 )
        {
            LogError( ( "Failed to initialize AES-CMAC operation. "
                        "mbedtls_md_setup failed: mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            prvCMACCleanUp( pxSession );
            xResult = CKR_KEY_HANDLE_INVALID;
        }
    }

    if( xResult == CKR_OK )
    {
        pxSession->xCMACKeyHandle = hKey;
    }

    return xResult;
}

/**
 * @brief Helper function for initializing a sign operation for AES-CMAC.
 * @param[in] pxSession         Pointer to a valid PKCS #11 session.
 * @param[in] hKey              CMAC secret key handle.
 * @param[in] pucKeyData        CMAC secret key data.
 * @param[in] ulKeyDataLength   CMAC key Size.
 */
static CK_RV prvSignInitAESCMAC( P11Session_t * pxSession,
                                 CK_OBJECT_HANDLE hKey,
                                 CK_BYTE_PTR pucKeyData,
                                 CK_ULONG ulKeyDataLength )
{
    CK_RV xResult = CKR_OK;

    xResult = prvInitAESCMAC( pxSession,
                              hKey,
                              pucKeyData,
                              ulKeyDataLength );

    return xResult;
}

/**
 * @brief Helper function for cleaning up a sign operation for an EC or RSA key.
 * @param[in] pxSession   Pointer to a valid PKCS #11 session.
 */
static void prvSignInitEC_RSACleanUp( P11Session_t * pxSession )
{
    mbedtls_pk_free( &pxSession->xSignKey );
    pxSession->xSignKeyHandle = CK_INVALID_HANDLE;
}


/**
 * @brief Helper function for initializing a sign operation for an EC or RSA key.
 * @param[in] pxSession   Pointer to a valid PKCS #11 session.
 * @param[in] pMechanism  EC/RSA mechanism.
 * @param[in] hKey        EC/RSA private key handle.
 * @param[in] pucKeyData        EC/RSA public key data.
 * @param[in] ulKeyDataLength   EC/RSA public key size.
 */
static CK_RV prvSignInitEC_RSAKeys( P11Session_t * pxSession,
                                    CK_MECHANISM_PTR pMechanism,
                                    CK_OBJECT_HANDLE hKey,
                                    CK_BYTE_PTR pucKeyData,
                                    CK_ULONG ulKeyDataLength )
{
    mbedtls_pk_type_t xKeyType;
    int32_t lMbedTLSResult = 0;
    CK_RV xResult = CKR_KEY_HANDLE_INVALID;

    mbedtls_pk_init( &pxSession->xSignKey );

    #if MBEDTLS_VERSION_NUMBER < 0x03000000
        lMbedTLSResult = mbedtls_pk_parse_key( &pxSession->xSignKey,
                                               pucKeyData, ulKeyDataLength,
                                               NULL, 0 );
    #else
        lMbedTLSResult = mbedtls_pk_parse_key( &pxSession->xSignKey,
                                               pucKeyData, ulKeyDataLength,
                                               NULL, 0,
                                               mbedtls_ctr_drbg_random, &xP11Context.xMbedDrbgCtx );
    #endif /* MBEDTLS_VERSION_NUMBER < 0x03000000 */

    if( 0 == lMbedTLSResult )
    {
        pxSession->xSignKeyHandle = hKey;
        xResult = CKR_OK;
    }
    else
    {
        LogError( ( "Failed to initialize sign operation. "
                    "mbedtls_pk_parse_key failed: mbed TLS "
                    "error = %s : %s.",
                    mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                    mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
        prvSignInitEC_RSACleanUp( pxSession );
    }

    /* Check that the mechanism and key type are compatible, supported. */
    if( xResult == CKR_OK )
    {
        xKeyType = mbedtls_pk_get_type( &pxSession->xSignKey );

        if( ( pMechanism->mechanism == CKM_RSA_PKCS ) && ( xKeyType == MBEDTLS_PK_RSA ) )
        {
            /* Mechanisms align with the port. */
        }
        else if( ( pMechanism->mechanism == CKM_ECDSA ) && ( ( xKeyType == MBEDTLS_PK_ECDSA ) || ( xKeyType == MBEDTLS_PK_ECKEY ) ) )
        {
            /* Mechanisms align with the port. */
        }
        else
        {
            LogError( ( "Failed to initialize sign operation. "
                        "Signing key type (0x%0lX) does not match "
                        "RSA or EC mechanism.",
                        ( unsigned long int ) xKeyType ) );
            xResult = CKR_KEY_TYPE_INCONSISTENT;
            prvSignInitEC_RSACleanUp( pxSession );
        }
    }

    return xResult;
}

/**
 * @brief Initializes a signature operation.
 *
 * \sa C_Sign() completes signatures initiated by C_SignInit().
 *
 * \note C_Sign() parameters are shared by a session.  Calling
 * C_SignInit() & C_Sign() with the same session across different
 * tasks may lead to unexpected results.
 *
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 * @param[in] pMechanism                    Mechanism used to sign.
 *                                          This port supports the following mechanisms:
 *                                          - CKM_RSA_PKCS for RSA signatures
 *                                          - CKM_ECDSA for elliptic curve signatures
 *                                          Note that neither of these mechanisms perform
 *                                          hash operations.
 * @param[in] hKey                          The handle of the private key to be used for
 *                                          signature. Key must be compatible with the
 *                                          mechanism chosen by pMechanism.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_signinit] */
CK_DECLARE_FUNCTION( CK_RV, C_SignInit )( CK_SESSION_HANDLE hSession,
                                          CK_MECHANISM_PTR pMechanism,
                                          CK_OBJECT_HANDLE hKey )
{
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xIsPrivate = ( CK_BBOOL ) CK_TRUE;
    CK_OBJECT_HANDLE xPalHandle = CK_INVALID_HANDLE;
    CK_BYTE_PTR pxLabel = NULL;
    CK_ULONG xLabelLength = 0;

    CK_BYTE_PTR pucKeyData = NULL;
    CK_ULONG ulKeyDataLength = 0;

    P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );

    if( NULL == pMechanism )
    {
        LogError( ( "Failed to initialize sign operation. NULL pointer to "
                    "signing mechanism provided." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    if( ( xResult == CKR_OK ) && ( prvOperationActive( pxSession ) == ( CK_BBOOL ) CK_TRUE ) )
    {
        LogError( ( "Failed to initialize sign operation. Operation already active." ) );
        xResult = CKR_OPERATION_ACTIVE;
    }

    /* Retrieve key value from storage. */
    if( xResult == CKR_OK )
    {
        prvFindObjectInListByHandle( hKey,
                                     &xPalHandle,
                                     &pxLabel,
                                     &xLabelLength );

        if( xPalHandle != CK_INVALID_HANDLE )
        {
            xResult = PKCS11_PAL_GetObjectValue( xPalHandle, &pucKeyData, &ulKeyDataLength, &xIsPrivate );

            if( xResult != CKR_OK )
            {
                LogError( ( "Failed to initialize sign operation. Unable to "
                            "retrieve value of private key for signing 0x%0lX.", ( unsigned long int ) xResult ) );
                xResult = CKR_KEY_HANDLE_INVALID;
            }
        }
        else
        {
            LogDebug( ( "Could not find PKCS #11 PAL Handle." ) );
            xResult = CKR_KEY_HANDLE_INVALID;
        }
    }

    /* Check that a private key was retrieved. */
    if( xResult == CKR_OK )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        if( xIsPrivate != ( CK_BBOOL ) CK_TRUE )
        {
            LogError( ( "Failed to initialize sign operation. Sign operation "
                        "attempted with public key." ) );
            xResult = CKR_KEY_TYPE_INCONSISTENT;
        }
    }

    /* Convert the private key from storage format to mbedTLS usable format. */
    if( xResult == CKR_OK )
    {
        if( 0 == mbedtls_mutex_lock( &pxSession->xSignMutex ) )
        {
            switch( pMechanism->mechanism )
            {
                case CKM_RSA_PKCS:
                case CKM_ECDSA:

                    if( ( pxSession->xSignKeyHandle == CK_INVALID_HANDLE ) || ( pxSession->xSignKeyHandle != hKey ) )
                    {
                        xResult = prvSignInitEC_RSAKeys( pxSession, pMechanism, hKey, pucKeyData, ulKeyDataLength );
                    }
                    else
                    {
                        /* The correct credentials are already initialized. */
                    }

                    break;

                case CKM_SHA256_HMAC:

                    if( ( pxSession->xHMACKeyHandle == CK_INVALID_HANDLE ) || ( pxSession->xHMACKeyHandle != hKey ) )
                    {
                        xResult = prvSignInitSHA256HMAC( pxSession, hKey, pucKeyData, ulKeyDataLength );
                    }
                    else
                    {
                        /* The correct credentials are already initialized. */
                    }

                    break;

                case CKM_AES_CMAC:

                    if( ( pxSession->xCMACKeyHandle == CK_INVALID_HANDLE ) || ( pxSession->xCMACKeyHandle != hKey ) )
                    {
                        xResult = prvSignInitAESCMAC( pxSession, hKey, pucKeyData, ulKeyDataLength );
                    }
                    else
                    {
                        /* The correct credentials are already initialized. */
                    }

                    break;

                default:
                    LogError( ( "Failed to initialize sign operation. Received "
                                "an unknown or invalid mechanism." ) );
                    xResult = CKR_MECHANISM_INVALID;
                    break;
            }

            ( void ) mbedtls_mutex_unlock( &pxSession->xSignMutex );
        }
        else
        {
            LogError( ( "Failed sign operation. Could not take sign mutex." ) );
            xResult = CKR_CANT_LOCK;
        }
    }

    if( xPalHandle != CK_INVALID_HANDLE )
    {
        PKCS11_PAL_GetObjectValueCleanup( pucKeyData, ulKeyDataLength );
    }

    if( xResult == CKR_OK )
    {
        LogDebug( ( "Sign mechanism set to 0x%0lX.", ( unsigned long int ) pMechanism->mechanism ) );
        pxSession->xOperationSignMechanism = pMechanism->mechanism;
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_signinit] */

/**
 * @brief Signs single-part data.
 *
 * \sa C_SignInit() initiates signatures signature creation.
 *
 * \note C_Sign() parameters are shared by a session.  Calling
 * C_SignInit() & C_Sign() with the same session across different
 * tasks may lead to unexpected results.
 *
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 * @param[in] pData                         Data to be signed.
 *                                          Note: Some applications may require this data to
 *                                          be hashed before passing to C_Sign().
 * @param[in] ulDataLen                     Length of pucData, in bytes.
 * @param[out] pSignature                   Buffer where signature will be placed.
 *                                          Caller is responsible for allocating memory.
 *                                          Providing NULL for this input will cause
 *                                          pulSignatureLen to be updated for length of
 *                                          buffer required.
 * @param[in,out] pulSignatureLen           Length of pucSignature buffer.
 *                                          If pucSignature is non-NULL, pulSignatureLen is
 *                                          updated to contain the actual signature length.
 *                                          If pucSignature is NULL, pulSignatureLen is
 *                                          updated to the buffer length required for signature
 *                                          data.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_sign] */
CK_DECLARE_FUNCTION( CK_RV, C_Sign )( CK_SESSION_HANDLE hSession,
                                      CK_BYTE_PTR pData,
                                      CK_ULONG ulDataLen,
                                      CK_BYTE_PTR pSignature,
                                      CK_ULONG_PTR pulSignatureLen )
{
    P11Session_t * pxSessionObj = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSessionObj );

    size_t xSignatureLength = 0;
    size_t xExpectedInputLength = 0;
    size_t xSignatureBufferLength = 0;
    CK_BYTE_PTR pucSignatureBuffer = pSignature;
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xSignatureGenerated = ( CK_BBOOL ) CK_FALSE;

    /* 8 bytes added to hold ASN.1 encoding information. */
    /* MISRA Ref 12.1.1 [Operator precedence] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-121 */
    /* coverity[misra_c_2012_rule_12_1_violation] */
    uint8_t ecSignature[ MBEDTLS_ECDSA_MAX_SIG_LEN( 256 ) ];

    int32_t lMbedTLSResult = -1;
    mbedtls_md_type_t xHashType = MBEDTLS_MD_NONE;

    if( ( NULL == pulSignatureLen ) || ( NULL == pData ) )
    {
        LogError( ( "Failed sign operation. Received a NULL pointer." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( CKR_OK == xResult )
    {
        /* Update the signature length. */
        if( pxSessionObj->xOperationSignMechanism == CKM_RSA_PKCS )
        {
            xSignatureLength = pkcs11RSA_2048_SIGNATURE_LENGTH;
            xSignatureBufferLength = pkcs11RSA_2048_SIGNATURE_LENGTH;
            xExpectedInputLength = pkcs11RSA_SIGNATURE_INPUT_LENGTH;
        }
        else if( pxSessionObj->xOperationSignMechanism == CKM_ECDSA )
        {
            xSignatureLength = pkcs11ECDSA_P256_SIGNATURE_LENGTH;
            /* MISRA Ref 12.1.1 [Operator precedence] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-121 */
            /* coverity[misra_c_2012_rule_12_1_violation] */
            xSignatureBufferLength = MBEDTLS_ECDSA_MAX_SIG_LEN( 256 );
            pucSignatureBuffer = ecSignature;
            xExpectedInputLength = pkcs11SHA256_DIGEST_LENGTH;
            xHashType = MBEDTLS_MD_SHA256;
        }
        else if( pxSessionObj->xOperationSignMechanism == CKM_SHA256_HMAC )
        {
            xSignatureLength = pkcs11SHA256_DIGEST_LENGTH;
            /* At least 32 bytes, can be longer. */
            xExpectedInputLength = pkcs11SHA256_DIGEST_LENGTH;
            xHashType = MBEDTLS_MD_SHA256;
        }
        else if( pxSessionObj->xOperationSignMechanism == CKM_AES_CMAC )
        {
            xSignatureLength = pkcs11AES_CMAC_SIGNATURE_LENGTH;
            xExpectedInputLength = pkcs11AES_CMAC_SIGNATURE_LENGTH;
        }
        else
        {
            LogError( ( "Failed sign operation. The sign operation was not "
                        "initialized with a call to C_SignInit." ) );
            xResult = CKR_OPERATION_NOT_INITIALIZED;
        }
    }

    if( xResult == CKR_OK )
    {
        /* Calling application is trying to determine length needed for signature buffer. */
        if( NULL != pSignature )
        {
            /* Check that the signature buffer is long enough. */
            if( *pulSignatureLen < xSignatureLength )
            {
                LogError( ( "Failed sign operation. The signature buffer was "
                            "too small. Expected: %lu bytes and received %lu "
                            "bytes.",
                            ( unsigned long int ) xSignatureLength,
                            ( unsigned long int ) *pulSignatureLen ) );
                xResult = CKR_BUFFER_TOO_SMALL;
            }

            if( CKR_OK == xResult )
            {
                if( 0 == mbedtls_mutex_lock( &pxSessionObj->xSignMutex ) )
                {
                    if( pxSessionObj->xOperationSignMechanism == CKM_SHA256_HMAC )
                    {
                        lMbedTLSResult = mbedtls_md_hmac_update( &pxSessionObj->xHMACSecretContext, pData, ulDataLen );

                        if( lMbedTLSResult == 0 )
                        {
                            lMbedTLSResult = mbedtls_md_hmac_finish( &pxSessionObj->xHMACSecretContext, pSignature );
                        }

                        prvHMACCleanUp( pxSessionObj );
                    }
                    else if( pxSessionObj->xOperationSignMechanism == CKM_AES_CMAC )
                    {
                        lMbedTLSResult = mbedtls_cipher_cmac_update( &pxSessionObj->xCMACSecretContext, pData, ulDataLen );

                        if( lMbedTLSResult == 0 )
                        {
                            lMbedTLSResult = mbedtls_cipher_cmac_finish( &pxSessionObj->xCMACSecretContext, pSignature );
                        }

                        prvCMACCleanUp( pxSessionObj );
                    }
                    else
                    {
                        /* Check that input data to be signed is the expected length. */
                        if( xExpectedInputLength > ulDataLen )
                        {
                            LogError( ( "Failed sign operation. The data buffer was "
                                        "too small. Expected at least %lu bytes and received "
                                        "%lu bytes.",
                                        ( unsigned long int ) xExpectedInputLength,
                                        ( unsigned long int ) ulDataLen ) );
                            xResult = CKR_DATA_LEN_RANGE;
                        }
                        else
                        {
                            LogDebug( ( "Ready to sign: xSignatureLength=%lu *pulSignatureLen=%lu", ( unsigned long int ) xSignatureLength, *pulSignatureLen ) );

                            /* Per mbed TLS documentation, if using RSA, md_alg should
                             * be MBEDTLS_MD_NONE. If ECDSA, md_alg should never be
                             * MBEDTLS_MD_NONE. SHA-256 will be used for ECDSA for
                             * consistency with the rest of the port.
                             */
                            #if MBEDTLS_VERSION_NUMBER < 0x03000000
                                lMbedTLSResult = mbedtls_pk_sign( &pxSessionObj->xSignKey,
                                                                  xHashType,
                                                                  pData, ulDataLen,
                                                                  pucSignatureBuffer,
                                                                  &xSignatureBufferLength,
                                                                  mbedtls_ctr_drbg_random,
                                                                  &xP11Context.xMbedDrbgCtx );
                            #else
                                lMbedTLSResult = mbedtls_pk_sign( &pxSessionObj->xSignKey,
                                                                  xHashType,
                                                                  pData, ulDataLen,
                                                                  pucSignatureBuffer,
                                                                  xSignatureBufferLength,
                                                                  &xSignatureBufferLength,
                                                                  mbedtls_ctr_drbg_random,
                                                                  &xP11Context.xMbedDrbgCtx );
                            #endif /* MBEDTLS_VERSION_NUMBER < 0x03000000 */
                        }

                        prvSignInitEC_RSACleanUp( pxSessionObj );
                    }

                    if( ( xResult == CKR_OK ) && ( lMbedTLSResult != 0 ) )
                    {
                        LogError( ( "Failed sign operation. mbed TLS error = %s : %s.",
                                    mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                    mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                        xResult = CKR_FUNCTION_FAILED;
                    }

                    ( void ) mbedtls_mutex_unlock( &pxSessionObj->xSignMutex );
                    /* MISRA Ref 10.5.1 [Essential type casting] */
                    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                    /* coverity[misra_c_2012_rule_10_5_violation] */
                    xSignatureGenerated = ( CK_BBOOL ) CK_TRUE;
                }
                else
                {
                    LogError( ( "Failed sign operation. Could not take xSignMutex." ) );
                    xResult = CKR_CANT_LOCK;
                }
            }
        }
        else
        {
            *pulSignatureLen = xSignatureLength;
        }
    }

    if( xResult == CKR_OK )
    {
        /* MISRA Ref 10.5.1 [Essential type casting] */
        /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
        /* coverity[misra_c_2012_rule_10_5_violation] */
        if( ( pxSessionObj->xOperationSignMechanism == CKM_ECDSA ) && ( xSignatureGenerated == ( CK_BBOOL ) CK_TRUE ) )
        {
            /* If this an EC signature, reformat from ASN.1 encoded to 64-byte R & S components */
            lMbedTLSResult = PKI_mbedTLSSignatureToPkcs11Signature( pSignature, ecSignature );

            if( lMbedTLSResult != 0 )
            {
                LogError( ( "Failed sign operation. Failed to convert from "
                            "ASN.1 encoding to 64 byte R & S components." ) );
                xResult = CKR_FUNCTION_FAILED;
            }
        }
    }

    if( ( xResult == CKR_OK ) || ( xResult == CKR_BUFFER_TOO_SMALL ) )
    {
        *pulSignatureLen = xSignatureLength;
    }

    /* Complete the operation in the context. */
    if( ( xResult != CKR_BUFFER_TOO_SMALL ) && ( xResult != CKR_SESSION_HANDLE_INVALID ) )
    {
        LogDebug( ( "Ended Sign operation." ) );
        pxSessionObj->xOperationSignMechanism = pkcs11NO_OPERATION;
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_sign] */

/**
 * @brief Helper function for initializing a verify operation for SHA256-HMAC.
 * @param[in] pxSession         Pointer to a valid PKCS #11 session.
 * @param[in] hKey              HMAC secret key handle.
 * @param[in] pucKeyData        HMAC secret key data.
 * @param[in] ulKeyDataLength   HMAC key Size.
 */
static CK_RV prvVerifyInitSHA256HMAC( P11Session_t * pxSession,
                                      CK_OBJECT_HANDLE hKey,
                                      CK_BYTE_PTR pucKeyData,
                                      CK_ULONG ulKeyDataLength )
{
    CK_RV xResult = CKR_OK;

    xResult = prvInitSHA256HMAC( pxSession,
                                 hKey,
                                 pucKeyData,
                                 ulKeyDataLength );

    return xResult;
}

/**
 * @brief Helper function for initializing a verify operation for AES-CMAC.
 * @param[in] pxSession         Pointer to a valid PKCS #11 session.
 * @param[in] hKey              CMAC secret key handle.
 * @param[in] pucKeyData        CMAC secret key data.
 * @param[in] ulKeyDataLength   CMAC key Size.
 */
static CK_RV prvVerifyInitAESCMAC( P11Session_t * pxSession,
                                   CK_OBJECT_HANDLE hKey,
                                   CK_BYTE_PTR pucKeyData,
                                   CK_ULONG ulKeyDataLength )
{
    CK_RV xResult = CKR_OK;

    xResult = prvInitAESCMAC( pxSession,
                              hKey,
                              pucKeyData,
                              ulKeyDataLength );

    return xResult;
}

/**
 * @brief Helper function for cleaning up a verify operation for an EC or RSA key.
 * @param[in] pxSession   Pointer to a valid PKCS #11 session.
 */
static void prvVerifyInitEC_RSACleanUp( P11Session_t * pxSession )
{
    mbedtls_pk_free( &pxSession->xVerifyKey );
    pxSession->xVerifyKeyHandle = CK_INVALID_HANDLE;
}


/**
 * @brief Helper function for initializing a verify operation for an EC or RSA key.
 * @param[in] pxSession   Pointer to a valid PKCS #11 session.
 * @param[in] pMechanism  EC/RSA mechanism.
 * @param[in] hKey        EC/RSA public key handle.
 * @param[in] pucKeyData        EC/RSA public key data.
 * @param[in] ulKeyDataLength   EC/RSA public key size.
 */
static CK_RV prvVerifyInitEC_RSAKeys( P11Session_t * pxSession,
                                      CK_MECHANISM_PTR pMechanism,
                                      CK_OBJECT_HANDLE hKey,
                                      CK_BYTE_PTR pucKeyData,
                                      CK_ULONG ulKeyDataLength )
{
    mbedtls_pk_type_t xKeyType;
    int32_t lMbedTLSResult = 1;
    CK_RV xResult = CKR_KEY_HANDLE_INVALID;

    mbedtls_pk_init( &pxSession->xVerifyKey );
    lMbedTLSResult = mbedtls_pk_parse_public_key( &pxSession->xVerifyKey, pucKeyData, ulKeyDataLength );

    if( 0 == lMbedTLSResult )
    {
        pxSession->xVerifyKeyHandle = hKey;
        xResult = CKR_OK;
    }

    /* If we fail to parse the public key, try again as a private key. */
    if( xResult != CKR_OK )
    {
        #if MBEDTLS_VERSION_NUMBER < 0x03000000
            lMbedTLSResult = mbedtls_pk_parse_key( &pxSession->xVerifyKey,
                                                   pucKeyData, ulKeyDataLength,
                                                   NULL, 0 );
        #else
            lMbedTLSResult = mbedtls_pk_parse_key( &pxSession->xVerifyKey,
                                                   pucKeyData, ulKeyDataLength,
                                                   NULL, 0,
                                                   mbedtls_ctr_drbg_random, &xP11Context.xMbedDrbgCtx );
        #endif /* MBEDTLS_VERSION_NUMBER < 0x03000000 */

        if( 0 == lMbedTLSResult )
        {
            pxSession->xVerifyKeyHandle = hKey;
            xResult = CKR_OK;
        }
        else
        {
            LogError( ( "Verification operation failed. "
                        "mbedtls_pk_parse_key failed: mbed TLS "
                        "error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            prvVerifyInitEC_RSACleanUp( pxSession );
        }
    }

    /* Check that the mechanism and key type are compatible and supported. */
    if( xResult == CKR_OK )
    {
        xKeyType = mbedtls_pk_get_type( &pxSession->xVerifyKey );

        if( ( ( pMechanism->mechanism == CKM_RSA_PKCS ) || ( pMechanism->mechanism == CKM_RSA_X_509 ) ) && ( xKeyType == MBEDTLS_PK_RSA ) )
        {
            /* Mechanisms align with the port. */
        }
        else if( ( pMechanism->mechanism == CKM_ECDSA ) &&
                 ( ( xKeyType == MBEDTLS_PK_ECDSA ) || ( xKeyType == MBEDTLS_PK_ECKEY ) ) )
        {
            /* Mechanisms align with the port. */
        }
        else
        {
            LogError( ( "Failed to initialize verify operation. "
                        "Verification key type (0x%0lX) does not match "
                        "RSA or EC mechanism.",
                        ( unsigned long int ) xKeyType ) );
            xResult = CKR_KEY_TYPE_INCONSISTENT;
            prvVerifyInitEC_RSACleanUp( pxSession );
        }
    }

    return xResult;
}

/**
 * @brief Initializes a verification operation.
 *
 * \sa C_Verify() completes verifications initiated by C_VerifyInit().
 *
 * \note C_Verify() parameters are shared by a session.  Calling
 * C_VerifyInit() & C_Verify() with the same session across different
 * tasks may lead to unexpected results.
 *
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 * @param[in] pMechanism                    Mechanism used to verify signature.
 *                                          This port supports the following mechanisms:
 *                                          - CKM_RSA_X_509 for RSA verifications
 *                                          - CKM_ECDSA for elliptic curve verifications
 * @param[in] hKey                          The handle of the public key to be used for
 *                                          verification. Key must be compatible with the
 *                                          mechanism chosen by pxMechanism.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_verifyinit] */
CK_DECLARE_FUNCTION( CK_RV, C_VerifyInit )( CK_SESSION_HANDLE hSession,
                                            CK_MECHANISM_PTR pMechanism,
                                            CK_OBJECT_HANDLE hKey )
{
    P11Session_t * pxSession;
    CK_RV xResult = CKR_OK;
    CK_OBJECT_HANDLE xPalHandle = CK_INVALID_HANDLE;
    CK_BYTE_PTR pxLabel = NULL;
    CK_ULONG xLabelLength = 0;
    CK_BYTE_PTR pucKeyData = NULL;
    CK_ULONG ulKeyDataLength = 0;

    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xIsPrivate = ( CK_BBOOL ) CK_TRUE;

    pxSession = prvSessionPointerFromHandle( hSession );
    xResult = prvCheckValidSessionAndModule( pxSession );

    if( NULL == pMechanism )
    {
        LogError( ( "Failed to initialize verify operation. Null verification "
                    "mechanism provided." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    if( ( xResult == CKR_OK ) && ( prvOperationActive( pxSession ) == ( CK_BBOOL ) CK_TRUE ) )
    {
        LogError( ( "Failed to initialize verify operation. An operation was "
                    "already active." ) );
        xResult = CKR_OPERATION_ACTIVE;
    }

    if( xResult == CKR_OK )
    {
        prvFindObjectInListByHandle( hKey,
                                     &xPalHandle,
                                     &pxLabel,
                                     &xLabelLength );

        if( xPalHandle != CK_INVALID_HANDLE )
        {
            xResult = PKCS11_PAL_GetObjectValue( xPalHandle, &pucKeyData, &ulKeyDataLength, &xIsPrivate );

            if( xResult != CKR_OK )
            {
                LogError( ( "Failed to initialize verify operation. Unable to "
                            "retrieve value of public key for verification 0x%0lX.",
                            ( unsigned long int ) xResult ) );
                xResult = CKR_KEY_HANDLE_INVALID;
            }
        }
        else
        {
            LogError( ( "Failed to initialize verify operation. Couldn't find "
                        "a valid PKCS #11 PAL Handle." ) );
            xResult = CKR_KEY_HANDLE_INVALID;
        }
    }

    /* Retrieve key value from storage. */
    if( xResult == CKR_OK )
    {
        /* Mutex is used to protect the verification keys and contexts. */
        if( 0 == mbedtls_mutex_lock( &pxSession->xVerifyMutex ) )
        {
            switch( pMechanism->mechanism )
            {
                case CKM_RSA_PKCS:
                case CKM_RSA_X_509:
                case CKM_ECDSA:

                    /* MISRA Ref 10.5.1 [Essential type casting] */
                    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
                    /* coverity[misra_c_2012_rule_10_5_violation] */
                    if( xIsPrivate != ( CK_BBOOL ) CK_FALSE )
                    {
                        LogError( ( "Failed to initialize verify operation. Verify "
                                    "operation attempted with private key." ) );
                        xResult = CKR_KEY_TYPE_INCONSISTENT;
                    }
                    else if( ( pxSession->xVerifyKeyHandle == CK_INVALID_HANDLE ) || ( pxSession->xVerifyKeyHandle != hKey ) )
                    {
                        xResult = prvVerifyInitEC_RSAKeys( pxSession, pMechanism, hKey, pucKeyData, ulKeyDataLength );
                    }
                    else
                    {
                        /* The correct credentials are already Initialized. */
                    }

                    break;

                case CKM_SHA256_HMAC:

                    if( ( pxSession->xHMACKeyHandle == CK_INVALID_HANDLE ) || ( pxSession->xHMACKeyHandle != hKey ) )
                    {
                        xResult = prvVerifyInitSHA256HMAC( pxSession, hKey, pucKeyData, ulKeyDataLength );
                    }

                    break;

                case CKM_AES_CMAC:

                    if( ( pxSession->xCMACKeyHandle == CK_INVALID_HANDLE ) || ( pxSession->xCMACKeyHandle != hKey ) )
                    {
                        xResult = prvVerifyInitAESCMAC( pxSession, hKey, pucKeyData, ulKeyDataLength );
                    }

                    break;

                default:
                    LogError( ( "Failed to initialize verify operation. Received "
                                "an unknown or invalid mechanism." ) );
                    xResult = CKR_MECHANISM_INVALID;
                    break;
            }

            ( void ) mbedtls_mutex_unlock( &pxSession->xVerifyMutex );
        }
        else
        {
            LogError( ( "Verify operation failed. Could not take verify mutex." ) );
            xResult = CKR_CANT_LOCK;
        }
    }

    if( xPalHandle != CK_INVALID_HANDLE )
    {
        PKCS11_PAL_GetObjectValueCleanup( pucKeyData, ulKeyDataLength );
    }

    if( xResult == CKR_OK )
    {
        LogDebug( ( "Verify mechanism set to 0x%0lX.", ( unsigned long int ) pMechanism->mechanism ) );
        pxSession->xOperationVerifyMechanism = pMechanism->mechanism;
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_verifyinit] */

/**
 * @brief Verifies a signature on single-part data.
 *
 * \note C_VerifyInit() must have been called previously.
 *
 * \note C_Verify() parameters are shared by a session.  Calling
 * C_VerifyInit() & C_Verify() with the same session across different
 * tasks may lead to unexpected results.
 *
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 * @param[in] pData                         Data who's signature is to be verified.
 *                                          Note: In this implementation, this is generally
 *                                          expected to be the hash of the data.
 * @param[in] ulDataLen                     Length of pucData.
 * @param[in] pSignature                    The signature to be verified.
 * @param[in] ulSignatureLen                Length of pucSignature in bytes.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_verify] */
CK_DECLARE_FUNCTION( CK_RV, C_Verify )( CK_SESSION_HANDLE hSession,
                                        CK_BYTE_PTR pData,
                                        CK_ULONG ulDataLen,
                                        CK_BYTE_PTR pSignature,
                                        CK_ULONG ulSignatureLen )
{
    P11Session_t * pxSessionObj;
    int32_t lMbedTLSResult;
    CK_RV xResult = CKR_OK;

    /* If using SHA512 a larger buffer is needed for the call to mbedtls_md_hmac_finish */
    #if defined( MBEDTLS_SHA512_C )
    CK_BYTE pxHMACBuffer[ pkcs11SHA256_DIGEST_LENGTH * 2 ] = { 0 };
    #else
    CK_BYTE pxHMACBuffer[ pkcs11SHA256_DIGEST_LENGTH ] = { 0 };
    #endif
    CK_BYTE pxCMACBuffer[ MBEDTLS_AES_BLOCK_SIZE ] = { 0 };

    pxSessionObj = prvSessionPointerFromHandle( hSession );
    xResult = prvCheckValidSessionAndModule( pxSessionObj );

    /* Check parameters. */
    if( ( NULL == pData ) ||
        ( NULL == pSignature ) )
    {
        LogError( ( "Failed verify operation. Received a NULL pointer." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    /* Check that the signature and data are the expected length.
     * These PKCS #11 mechanism expect data to be pre-hashed/formatted. */
    if( xResult == CKR_OK )
    {
        if( pxSessionObj->xOperationVerifyMechanism == CKM_RSA_X_509 )
        {
            if( ulDataLen != pkcs11SHA256_DIGEST_LENGTH )
            {
                LogError( ( "Failed verify operation. Data Length was too "
                            "short for pkcs11SHA256_DIGEST_LENGTH." ) );
                xResult = CKR_DATA_LEN_RANGE;
            }

            if( ulSignatureLen != pkcs11RSA_2048_SIGNATURE_LENGTH )
            {
                LogError( ( "Failed verify operation. Signature Length was too "
                            "short for pkcs11RSA_2048_SIGNATURE_LENGTH." ) );
                xResult = CKR_SIGNATURE_LEN_RANGE;
            }
        }
        else if( pxSessionObj->xOperationVerifyMechanism == CKM_RSA_PKCS )
        {
            if( ulDataLen != pkcs11SHA256_DIGEST_LENGTH )
            {
                LogError( ( "Failed verify operation. Data Length was too "
                            "short for pkcs11SHA256_DIGEST_LENGTH." ) );
                xResult = CKR_DATA_LEN_RANGE;
            }

            if( ulSignatureLen != pkcs11RSA_2048_SIGNATURE_LENGTH )
            {
                LogError( ( "Failed verify operation. Signature Length was too "
                            "short for pkcs11RSA_2048_SIGNATURE_LENGTH." ) );
                xResult = CKR_SIGNATURE_LEN_RANGE;
            }
        }
        else if( pxSessionObj->xOperationVerifyMechanism == CKM_ECDSA )
        {
            if( ulDataLen != pkcs11SHA256_DIGEST_LENGTH )
            {
                LogError( ( "Failed verify operation. Data Length was too "
                            "short for pkcs11SHA256_DIGEST_LENGTH." ) );
                xResult = CKR_DATA_LEN_RANGE;
            }

            if( ulSignatureLen != pkcs11ECDSA_P256_SIGNATURE_LENGTH )
            {
                LogError( ( "Failed verify operation. Data Length was too "
                            "short for pkcs11ECDSA_P256_SIGNATURE_LENGTH." ) );
                xResult = CKR_SIGNATURE_LEN_RANGE;
            }
        }
        else if( pxSessionObj->xOperationVerifyMechanism == CKM_SHA256_HMAC )
        {
            if( ulSignatureLen != pkcs11SHA256_DIGEST_LENGTH )
            {
                LogError( ( "Failed verify operation. Data Length was too "
                            "short for pkcs11SHA256_DIGEST_LENGTH." ) );
                xResult = CKR_SIGNATURE_LEN_RANGE;
            }
        }
        else if( pxSessionObj->xOperationVerifyMechanism == CKM_AES_CMAC )
        {
            if( ulSignatureLen != PKCS11_AES_CMAC_MIN_SIZE )
            {
                LogError( ( "Failed verify operation. Data Length was too "
                            "short for PKCS11_AES_CMAC_MIN_SIZE." ) );
                xResult = CKR_SIGNATURE_LEN_RANGE;
            }
        }
        else
        {
            LogError( ( "Failed verify operation. A C_Verify operation must be "
                        "initialized by a preceding call to C_VerifyInit. "
                        "This must happen before every call to C_Verify." ) );
            xResult = CKR_OPERATION_NOT_INITIALIZED;
        }
    }

    /* Verification step. */
    if( xResult == CKR_OK )
    {
        if( 0 == mbedtls_mutex_lock( &pxSessionObj->xVerifyMutex ) )
        {
            /* Perform an RSA verification. */
            if( pxSessionObj->xOperationVerifyMechanism == CKM_RSA_X_509 )
            {
                /* Verify the signature. If a public key is present, use it. */
                if( NULL != pxSessionObj->xVerifyKey.pk_ctx )
                {
                    lMbedTLSResult = mbedtls_pk_verify( &pxSessionObj->xVerifyKey,
                                                        MBEDTLS_MD_SHA256,
                                                        pData,
                                                        ulDataLen,
                                                        pSignature,
                                                        ulSignatureLen );

                    if( 0 != lMbedTLSResult )
                    {
                        LogError( ( "Failed verify operation. mbedtls_pk_verify "
                                    "failed: mbed TLS error = %s : %s.",
                                    mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                    mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                        xResult = CKR_SIGNATURE_INVALID;
                    }
                }
                else
                {
                    LogError( ( "Failed verify operation. Verify Key was not "
                                "present in session context." ) );

                    xResult = CKR_SIGNATURE_INVALID;
                }

                prvVerifyInitEC_RSACleanUp( pxSessionObj );
            }
            /* Perform an ECDSA verification. */
            else if( pxSessionObj->xOperationVerifyMechanism == CKM_ECDSA )
            {
                /* An ECDSA signature is comprised of 2 components - R & S.  C_Sign returns them one after another. */
                mbedtls_ecdsa_context * pxEcdsaContext;
                mbedtls_mpi xR;
                mbedtls_mpi xS;
                mbedtls_mpi_init( &xR );
                mbedtls_mpi_init( &xS );

                lMbedTLSResult = mbedtls_mpi_read_binary( &xR, &( pSignature[ 0 ] ), 32 );

                if( lMbedTLSResult != 0 )
                {
                    xResult = CKR_SIGNATURE_INVALID;
                    LogError( ( "Failed verify operation. Failed to parse R in EC "
                                "signature: mbed TLS error = %s : %s.",
                                mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                }
                else
                {
                    lMbedTLSResult = mbedtls_mpi_read_binary( &xS, &( pSignature[ 32 ] ), 32 );

                    if( lMbedTLSResult != 0 )
                    {
                        xResult = CKR_SIGNATURE_INVALID;
                        LogError( ( "Failed verify operation. Failed to parse S in "
                                    "EC signature: mbed TLS error = %s : %s.",
                                    mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                    mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                    }
                }

                if( xResult == CKR_OK )
                {
                    /* Verify the signature. If a public key is present, use it. */
                    if( NULL != pxSessionObj->xVerifyKey.pk_ctx )
                    {
                        pxEcdsaContext = pxSessionObj->xVerifyKey.pk_ctx;
                        lMbedTLSResult = mbedtls_ecdsa_verify( &pxEcdsaContext->grp, pData, ulDataLen, &pxEcdsaContext->Q, &xR, &xS );
                    }

                    if( lMbedTLSResult != 0 )
                    {
                        xResult = CKR_SIGNATURE_INVALID;
                        LogError( ( "Failed verify operation. "
                                    "mbedtls_ecdsa_verify failed: mbed TLS error = %s : %s.",
                                    mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                    mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                    }
                }

                mbedtls_mpi_free( &xR );
                mbedtls_mpi_free( &xS );
                prvVerifyInitEC_RSACleanUp( pxSessionObj );
            }
            else if( pxSessionObj->xOperationVerifyMechanism == CKM_SHA256_HMAC )
            {
                lMbedTLSResult = mbedtls_md_hmac_update( &pxSessionObj->xHMACSecretContext, pData, ulDataLen );

                if( lMbedTLSResult != 0 )
                {
                    xResult = CKR_SIGNATURE_INVALID;
                    LogError( ( "Failed verify operation. "
                                "mbedtls_md_hmac_update failed: mbed TLS error = %s : %s.",
                                mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                }
                else
                {
                    lMbedTLSResult = mbedtls_md_hmac_finish( &pxSessionObj->xHMACSecretContext, pxHMACBuffer );

                    if( lMbedTLSResult != 0 )
                    {
                        xResult = CKR_SIGNATURE_INVALID;
                        LogError( ( "Failed verify operation. "
                                    "mbedtls_md_hmac_finish failed: mbed TLS error = %s : %s.",
                                    mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                    mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                    }
                    else
                    {
                        if( 0 != memcmp( pxHMACBuffer, pSignature, pkcs11SHA256_DIGEST_LENGTH ) )
                        {
                            xResult = CKR_SIGNATURE_INVALID;
                            LogError( ( "Failed verify operation. Signature was invalid." ) );
                        }
                    }
                }

                prvHMACCleanUp( pxSessionObj );
            }
            else if( pxSessionObj->xOperationVerifyMechanism == CKM_AES_CMAC )
            {
                lMbedTLSResult = mbedtls_cipher_cmac_update( &pxSessionObj->xCMACSecretContext, pData, ulDataLen );

                if( lMbedTLSResult != 0 )
                {
                    xResult = CKR_SIGNATURE_INVALID;
                    LogError( ( "Failed verify operation. "
                                "mbedtls_md_hmac_update failed: mbed TLS error = %s : %s.",
                                mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                }
                else
                {
                    lMbedTLSResult = mbedtls_cipher_cmac_finish( &pxSessionObj->xCMACSecretContext, pxCMACBuffer );

                    if( lMbedTLSResult != 0 )
                    {
                        xResult = CKR_SIGNATURE_INVALID;
                        LogError( ( "Failed verify operation. "
                                    "mbedtls_md_hmac_finish failed: mbed TLS error = %s : %s.",
                                    mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                                    mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
                    }
                    else
                    {
                        if( 0 != memcmp( pxCMACBuffer, pSignature, MBEDTLS_AES_BLOCK_SIZE ) )
                        {
                            xResult = CKR_SIGNATURE_INVALID;
                            LogError( ( "Failed verify operation. Signature was invalid." ) );
                        }
                    }
                }

                prvCMACCleanUp( pxSessionObj );
            }
            else
            {
                LogError( ( "Failed verify operation. Received an unexpected mechanism." ) );
            }

            ( void ) mbedtls_mutex_unlock( &pxSessionObj->xVerifyMutex );
        }
        else
        {
            LogError( ( "Failed to initialize verify operation. Could not "
                        "take xVerifyMutex." ) );
            xResult = CKR_CANT_LOCK;
        }
    }

    if( xResult != CKR_SESSION_HANDLE_INVALID )
    {
        LogDebug( ( "Reset Verify mechanism to pkcs11NO_OPERATION." ) );
        pxSessionObj->xOperationVerifyMechanism = pkcs11NO_OPERATION;
    }

    /* Return the signature verification result. */
    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_verify] */

/**
 * @brief Checks that the private key template provided for C_GenerateKeyPair
 * contains all necessary attributes, and does not contain any invalid
 * attributes.
 *
 * @param[out] ppxLabel                       Pointer to PKCS #11 label.
 * @param[in] pxAttribute                     PKCS #11 attribute to search.
 * @param[in] pulAttributeMap                 Flag to track whether all required attribute
 *                                            are in the key generation template.
 * @return CKR_OK if successful.
 */
static CK_RV prvCheckGenerateKeyPairPrivateTemplate( CK_ATTRIBUTE ** ppxLabel,
                                                     CK_ATTRIBUTE * pxAttribute,
                                                     uint32_t * pulAttributeMap )
{
    CK_RV xResult = CKR_OK;
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xBool = ( CK_BBOOL ) CK_FALSE;
    CK_ULONG xTemp = 0;

    switch( pxAttribute->type )
    {
        case ( CKA_LABEL ):
            *ppxLabel = pxAttribute;
            *pulAttributeMap |= LABEL_IN_TEMPLATE;
            break;

        case ( CKA_KEY_TYPE ):

            if( pxAttribute->ulValueLen == sizeof( CK_ULONG ) )
            {
                ( void ) memcpy( &xTemp, pxAttribute->pValue, sizeof( CK_ULONG ) );
            }

            if( xTemp != CKK_EC )
            {
                LogError( ( "Failed parsing private key template. Only EC key "
                            "pair generation is supported." ) );
                xResult = CKR_TEMPLATE_INCONSISTENT;
            }

            break;

        case ( CKA_SIGN ):

            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, sizeof( CK_BBOOL ) );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool != ( CK_BBOOL ) CK_TRUE )
            {
                LogError( ( "Failed parsing private key template. Generating "
                            "private keys that cannot sign is not supported." ) );
                xResult = CKR_TEMPLATE_INCONSISTENT;
            }

            LogDebug( ( "CKA_SIGN was in template." ) );
            *pulAttributeMap |= SIGN_IN_TEMPLATE;
            break;

        case ( CKA_PRIVATE ):

            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, sizeof( CK_BBOOL ) );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool != ( CK_BBOOL ) CK_TRUE )
            {
                LogError( ( "Failed parsing private key template. Private must "
                            "be set to true in order to generate a private key." ) );
                xResult = CKR_TEMPLATE_INCONSISTENT;
            }

            LogDebug( ( "CKA_PRIVATE was in template." ) );
            *pulAttributeMap |= PRIVATE_IN_TEMPLATE;
            break;

        case ( CKA_TOKEN ):

            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, sizeof( CK_BBOOL ) );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool != ( CK_BBOOL ) CK_TRUE )
            {
                LogError( ( "Failed parsing private key template. Generating "
                            "private keys that are false for attribute CKA_TOKEN "
                            "is not supported." ) );
                xResult = CKR_TEMPLATE_INCONSISTENT;
            }

            break;

        default:
            LogError( ( "Failed parsing private key template. Found an unknown "
                        "attribute type." ) );
            xResult = CKR_ATTRIBUTE_TYPE_INVALID;
            break;
    }

    return xResult;
}

/**
 * @brief  Checks that the public key template provided for C_GenerateKeyPair
 * contains all necessary attributes, and does not contain any invalid
 * attributes.
 *
 * @param[out] ppxLabel                       Pointer to PKCS #11 label.
 * @param[in] pxAttribute                     PKCS #11 attribute to search.
 * @param[in] pulAttributeMap                 Flag to track whether all required attribute
 *                                            are in the key generation template.
 *
 * @return CKR_OK if successful.
 */
static CK_RV prvCheckGenerateKeyPairPublicTemplate( CK_ATTRIBUTE ** ppxLabel,
                                                    CK_ATTRIBUTE * pxAttribute,
                                                    uint32_t * pulAttributeMap )
{
    CK_RV xResult = CKR_OK;
    /* MISRA Ref 10.5.1 [Essential type casting] */
    /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
    /* coverity[misra_c_2012_rule_10_5_violation] */
    CK_BBOOL xBool = ( CK_BBOOL ) CK_TRUE;
    CK_KEY_TYPE xKeyType = 0xFFFFFFFFUL;
    const CK_BYTE pxEcParams[] = pkcs11DER_ENCODED_OID_P256;
    const CK_BYTE * pxEcAttVal = NULL;

    switch( pxAttribute->type )
    {
        case ( CKA_LABEL ):
            *ppxLabel = pxAttribute;
            *pulAttributeMap |= LABEL_IN_TEMPLATE;
            break;

        case ( CKA_KEY_TYPE ):

            if( pxAttribute->ulValueLen == sizeof( CK_KEY_TYPE ) )
            {
                ( void ) memcpy( &xKeyType, ( CK_KEY_TYPE * ) pxAttribute->pValue, sizeof( CK_KEY_TYPE ) );
            }

            if( xKeyType != CKK_EC )
            {
                LogError( ( "Failed parsing public key template. Only EC key "
                            "pair generation is supported." ) );
                xResult = CKR_TEMPLATE_INCONSISTENT;
            }

            break;

        case ( CKA_EC_PARAMS ):
            pxEcAttVal = ( CK_BYTE * ) pxAttribute->pValue;

            if( pxAttribute->ulValueLen == sizeof( pxEcParams ) )
            {
                if( memcmp( pxEcParams, pxEcAttVal, sizeof( pxEcParams ) ) != 0 )
                {
                    LogError( ( "Failed parsing public key template. Only P-256 key "
                                "generation is supported." ) );
                    xResult = CKR_TEMPLATE_INCONSISTENT;
                }
            }

            *pulAttributeMap |= EC_PARAMS_IN_TEMPLATE;
            break;

        case ( CKA_VERIFY ):

            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, sizeof( CK_BBOOL ) );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool != ( CK_BBOOL ) CK_TRUE )
            {
                LogError( ( "Failed parsing public key template. Generating public "
                            "keys that have a value of CK_FALSE for attribute "
                            "CKA_VERIFY is not supported." ) );
                xResult = CKR_TEMPLATE_INCONSISTENT;
            }

            *pulAttributeMap |= VERIFY_IN_TEMPLATE;
            break;

        case ( CKA_TOKEN ):

            if( pxAttribute->ulValueLen == sizeof( CK_BBOOL ) )
            {
                ( void ) memcpy( &xBool, pxAttribute->pValue, sizeof( CK_BBOOL ) );
            }

            /* MISRA Ref 10.5.1 [Essential type casting] */
            /* More details at: https://github.com/FreeRTOS/corePKCS11/blob/main/MISRA.md#rule-105 */
            /* coverity[misra_c_2012_rule_10_5_violation] */
            if( xBool != ( CK_BBOOL ) CK_TRUE )
            {
                xResult = CKR_TEMPLATE_INCONSISTENT;
            }

            break;

        default:
            xResult = CKR_TEMPLATE_INCONSISTENT;
            break;
    }

    return xResult;
}


/**
 * @brief Generates a public-key/private-key pair.
 *
 * This port only supports generating elliptic curve P-256
 * key pairs.
 *
 * @param[in] hSession                      Handle of a valid PKCS #11 session.
 * @param[in] pMechanism                    Pointer to a mechanism. At this time,
 *                                          CKM_EC_KEY_PAIR_GEN is the only supported mechanism.
 * @param[in] pPublicKeyTemplate           Pointer to a list of attributes that the generated
 *                                          public key should possess.
 *                                          Public key template must have the following attributes:
 *                                          - CKA_LABEL
 *                                              - Label should be no longer than pkcs11configMAX_LABEL_LENGTH
 *                                              and must be supported by port's PKCS #11 PAL.
 *                                          - CKA_EC_PARAMS
 *                                              - Must equal pkcs11DER_ENCODED_OID_P256.
 *                                              Only P-256 keys are supported.
 *                                          - CKA_VERIFY
 *                                              - Must be set to true.  Only public keys used
 *                                              for verification are supported.
 *                                          Public key templates may have the following attributes:
 *                                          - CKA_KEY_TYPE
 *                                              - Must be set to CKK_EC. Only elliptic curve key
 *                                              generation is supported.
 *                                          - CKA_TOKEN
 *                                              - Must be set to CK_TRUE.
 * @param[in] ulPublicKeyAttributeCount     Number of attributes in pPublicKeyTemplate.
 * @param[in] pPrivateKeyTemplate          Pointer to a list of attributes that the generated
 *                                          private key should possess.
 *                                          Private key template must have the following attributes:
 *                                          - CKA_LABEL
 *                                              - Label should be no longer than pkcs11configMAX_LABEL_LENGTH
 *                                              and must be supported by port's PKCS #11 PAL.
 *                                          - CKA_PRIVATE
 *                                              - Must be set to true.
 *                                          - CKA_SIGN
 *                                              - Must be set to true.  Only private keys used
 *                                              for signing are supported.
 *                                          Private key template may have the following attributes:
 *                                          - CKA_KEY_TYPE
 *                                              - Must be set to CKK_EC. Only elliptic curve key
 *                                              generation is supported.
 *                                          - CKA_TOKEN
 *                                              - Must be set to CK_TRUE.
 *
 * @param[in] ulPrivateKeyAttributeCount    Number of attributes in pPrivateKeyTemplate.
 * @param[out] phPublicKey                  Pointer to the handle of the public key to be created.
 * @param[out] phPrivateKey                 Pointer to the handle of the private key to be created.
 *
 * \note Not all attributes specified by the PKCS #11 standard are supported.
 * \note CKA_LOCAL attribute is not supported.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_generatekeypair] */
CK_DECLARE_FUNCTION( CK_RV, C_GenerateKeyPair )( CK_SESSION_HANDLE hSession,
                                                 CK_MECHANISM_PTR pMechanism,
                                                 CK_ATTRIBUTE_PTR pPublicKeyTemplate,
                                                 CK_ULONG ulPublicKeyAttributeCount,
                                                 CK_ATTRIBUTE_PTR pPrivateKeyTemplate,
                                                 CK_ULONG ulPrivateKeyAttributeCount,
                                                 CK_OBJECT_HANDLE_PTR phPublicKey,
                                                 CK_OBJECT_HANDLE_PTR phPrivateKey )
{
    uint8_t * pucDerFile = mbedtls_calloc( 1, pkcs11KEY_GEN_MAX_DER_SIZE );
    int32_t lMbedTLSResult = 0;
    uint32_t ulIndex = 0;
    mbedtls_pk_context xCtx = { 0 };
    CK_ATTRIBUTE_PTR pxPrivateLabel = NULL;
    CK_ATTRIBUTE_PTR pxPublicLabel = NULL;
    CK_OBJECT_HANDLE xPalPublic = CK_INVALID_HANDLE;
    CK_OBJECT_HANDLE xPalPrivate = CK_INVALID_HANDLE;
    uint32_t xPublicRequiredAttributeMap = ( LABEL_IN_TEMPLATE | EC_PARAMS_IN_TEMPLATE | VERIFY_IN_TEMPLATE );
    uint32_t xPrivateRequiredAttributeMap = ( LABEL_IN_TEMPLATE | PRIVATE_IN_TEMPLATE | SIGN_IN_TEMPLATE );
    uint32_t xAttributeMap = 0;
    CK_RV xAddObjectListResult = CKR_OK;

    const P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );
    CK_RV xResult = prvCheckValidSessionAndModule( pxSession );

    #ifdef pkcs11configSUPPRESS_ECDSA_MECHANISM
        if( xResult == CKR_OK )
        {
            LogDebug( ( "ECDSA Mechanism is suppressed on this port." ) );
            xResult = CKR_MECHANISM_INVALID;
        }
    #endif

    if( xResult == CKR_OK )
    {
        if( ( pPublicKeyTemplate == NULL ) ||
            ( pPrivateKeyTemplate == NULL ) ||
            ( phPublicKey == NULL ) ||
            ( phPrivateKey == NULL ) ||
            ( pMechanism == NULL ) )
        {
            LogError( ( "Failed generating a key pair. One of the arguments "
                        "was NULL." ) );
            xResult = CKR_ARGUMENTS_BAD;
        }
    }

    if( xResult == CKR_OK )
    {
        if( pucDerFile == NULL )
        {
            LogError( ( "Failed generating a key pair. Could not allocated a "
                        "buffer of size %u bytes.", ( unsigned int ) pkcs11KEY_GEN_MAX_DER_SIZE ) );
            xResult = CKR_HOST_MEMORY;
        }
    }

    if( xResult == CKR_OK )
    {
        if( CKM_EC_KEY_PAIR_GEN != pMechanism->mechanism )
        {
            LogError( ( "Failed generating a key pair. CKM_EC_KEY_PAIR_GEN is "
                        "the only valid key generation mechanism currently." ) );
            xResult = CKR_MECHANISM_INVALID;
        }
    }

    if( xResult == CKR_OK )
    {
        for( ulIndex = 0; ulIndex < ulPrivateKeyAttributeCount; ++ulIndex )
        {
            xResult = prvCheckGenerateKeyPairPrivateTemplate( &pxPrivateLabel,
                                                              &( pPrivateKeyTemplate[ ulIndex ] ),
                                                              &xAttributeMap );

            if( xResult != CKR_OK )
            {
                break;
            }
        }

        if( ( xResult == CKR_OK ) && ( ( xAttributeMap & xPrivateRequiredAttributeMap ) != xPrivateRequiredAttributeMap ) )
        {
            LogError( ( "Failed generating a key pair. Attributes were missing "
                        "in the private key template." ) );
            xResult = CKR_TEMPLATE_INCOMPLETE;
        }
    }

    if( xResult == CKR_OK )
    {
        xAttributeMap = 0;

        for( ulIndex = 0; ulIndex < ulPublicKeyAttributeCount; ++ulIndex )
        {
            xResult = prvCheckGenerateKeyPairPublicTemplate( &pxPublicLabel,
                                                             &( pPublicKeyTemplate[ ulIndex ] ),
                                                             &xAttributeMap );

            if( xResult != CKR_OK )
            {
                break;
            }
        }

        if( ( xResult == CKR_OK ) && ( ( xAttributeMap & xPublicRequiredAttributeMap ) != xPublicRequiredAttributeMap ) )
        {
            LogError( ( "Failed generating a key pair. Attributes were missing "
                        "in the public key template." ) );
            xResult = CKR_TEMPLATE_INCOMPLETE;
        }
    }

    if( xResult == CKR_OK )
    {
        mbedtls_pk_init( &xCtx );
        lMbedTLSResult = mbedtls_pk_setup( &xCtx, mbedtls_pk_info_from_type( MBEDTLS_PK_ECKEY ) );

        if( lMbedTLSResult != 0 )
        {
            LogError( ( "Failed generating a key pair. mbedtls_pk_setup failed: "
                        "mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            xResult = CKR_FUNCTION_FAILED;
        }
        else
        {
            LogDebug( ( "mbedtls_pk_setup was successful." ) );
        }
    }

    if( xResult == CKR_OK )
    {
        lMbedTLSResult = mbedtls_ecp_gen_key( MBEDTLS_ECP_DP_SECP256R1,
                                              mbedtls_pk_ec( xCtx ),
                                              mbedtls_ctr_drbg_random,
                                              &xP11Context.xMbedDrbgCtx );

        if( 0 != lMbedTLSResult )
        {
            LogError( ( "Failed generating a key pair. mbedtls_ecp_gen_key "
                        "failed: mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            xResult = CKR_FUNCTION_FAILED;
        }
        else
        {
            #if defined( GENERATED_PRIVATE_KEY_WRITE_PATH )
                char privatekey[ PRIV_KEY_BUFFER_LENGTH ];
                lMbedTLSResult = mbedtls_pk_write_key_pem( &xCtx, privatekey, PRIV_KEY_BUFFER_LENGTH );

                if( lMbedTLSResult == 0 )
                {
                    size_t privatekeyLength = strlen( privatekey );
                    FILE * fp = fopen( GENERATED_PRIVATE_KEY_WRITE_PATH, "w" );

                    if( NULL != fp )
                    {
                        const size_t writtenBytes = fwrite( privatekey, 1u, privatekeyLength, fp );

                        if( writtenBytes == privatekeyLength )
                        {
                            LogInfo( ( "Wrote the generated private key to %s successfully.", GENERATED_PRIVATE_KEY_WRITE_PATH ) );
                        }
                        else
                        {
                            LogError( ( "Could not write to %s. Error: %s.", GENERATED_PRIVATE_KEY_WRITE_PATH, strerror( errno ) ) );
                        }

                        fclose( fp );
                    }
                    else
                    {
                        LogError( ( "Could not open %s. Error: %s.", GENERATED_PRIVATE_KEY_WRITE_PATH, strerror( errno ) ) );
                    }
                }
            #else /* if defined( GENERATED_PRIVATE_KEY_WRITE_PATH ) */
                LogInfo( ( "NOTE: define GENERATED_PRIVATE_KEY_WRITE_PATH in order to have the private key written to disk." ) );
            #endif // GENERATED_PRIVATE_KEY_WRITE_PATH
        }
    }

    if( xResult == CKR_OK )
    {
        lMbedTLSResult = mbedtls_pk_write_pubkey_der( &xCtx, pucDerFile, pkcs11KEY_GEN_MAX_DER_SIZE );

        if( ( lMbedTLSResult > 0 ) &&
            ( lMbedTLSResult <= pkcs11KEY_GEN_MAX_DER_SIZE ) )
        {
            xPalPublic = PKCS11_PAL_SaveObject( pxPublicLabel, pucDerFile + pkcs11KEY_GEN_MAX_DER_SIZE - lMbedTLSResult, ( uint32_t ) lMbedTLSResult );
            LogDebug( ( "PKCS11_PAL_SaveObject returned a %lu PAL handle value "
                        "for the public key.", ( unsigned long int ) xPalPublic ) );
        }
        else
        {
            LogError( ( "Failed generating a key pair. "
                        "mbedtls_pk_write_pubkey_der failed: mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            xResult = CKR_GENERAL_ERROR;
        }
    }

    if( xResult == CKR_OK )
    {
        lMbedTLSResult = mbedtls_pk_write_key_der( &xCtx, pucDerFile, pkcs11KEY_GEN_MAX_DER_SIZE );

        if( ( lMbedTLSResult > 0 ) &&
            ( lMbedTLSResult <= pkcs11KEY_GEN_MAX_DER_SIZE ) )
        {
            xPalPrivate = PKCS11_PAL_SaveObject( pxPrivateLabel, pucDerFile + pkcs11KEY_GEN_MAX_DER_SIZE - lMbedTLSResult, ( uint32_t ) lMbedTLSResult );
            LogDebug( ( "PKCS11_PAL_SaveObject returned a %lu PAL handle value "
                        "for the private key.", ( unsigned long int ) xPalPrivate ) );
        }
        else
        {
            LogError( ( "Failed generating a key pair. mbedtls_pk_write_key_der "
                        "failed: mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            xResult = CKR_GENERAL_ERROR;
        }
    }

    if( ( xPalPublic != CK_INVALID_HANDLE ) && ( xPalPrivate != CK_INVALID_HANDLE ) )
    {
        xAddObjectListResult = prvAddObjectToList( xPalPrivate, phPrivateKey, pxPrivateLabel->pValue, pxPrivateLabel->ulValueLen );

        if( xAddObjectListResult == CKR_OK )
        {
            xAddObjectListResult = prvAddObjectToList( xPalPublic, phPublicKey, pxPublicLabel->pValue, pxPublicLabel->ulValueLen );
        }

        if( xAddObjectListResult != CKR_OK )
        {
            LogError( ( "Could not add private key to object list failed with (0x%0lX). Cleaning up PAL objects.", xResult ) );

            xResult = PKCS11_PAL_DestroyObject( xPalPrivate );

            if( xResult != CKR_OK )
            {
                LogError( ( "Could not clean up private key. PKCS11_PAL_DestroyObject failed with (0x%0lX).", xResult ) );
            }

            xResult = prvDeleteObjectFromList( xPalPrivate );

            if( xResult != CKR_OK )
            {
                LogError( ( "Could not remove private key object from internal list. Failed with (0x%0lX).", xResult ) );
            }

            xResult = PKCS11_PAL_DestroyObject( xPalPublic );

            if( xResult != CKR_OK )
            {
                LogError( ( "Could not clean up public key. PKCS11_PAL_DestroyObject failed with (0x%0lX).", xResult ) );
            }

            xResult = prvDeleteObjectFromList( xPalPublic );

            if( xResult != CKR_OK )
            {
                LogError( ( "Could not remove private key object from internal list. Failed with (0x%0lX).", xResult ) );
            }

            if( xResult == CKR_OK )
            {
                xResult = xAddObjectListResult;
            }
        }
    }

    /* Clean up. */
    mbedtls_free( pucDerFile );
    mbedtls_pk_free( &xCtx );

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_generatekeypair] */

/**
 * @brief Generates random data.
 *
 * @param[in] hSession          Handle of a valid PKCS #11 session.
 * @param[out] RandomData       Pointer to location that random data will be placed.
 *                              It is the responsibility of the application to allocate
 *                              this memory.
 * @param[in] ulRandomLen       Length of data (in bytes) to be generated.
 *
 * @return CKR_OK if successful.
 */
/* @[declare_pkcs11_mbedtls_c_generate_random] */
CK_DECLARE_FUNCTION( CK_RV, C_GenerateRandom )( CK_SESSION_HANDLE hSession,
                                                CK_BYTE_PTR RandomData,
                                                CK_ULONG ulRandomLen )
{
    CK_RV xResult = CKR_OK;
    int32_t lMbedTLSResult = 0;

    const P11Session_t * pxSession = prvSessionPointerFromHandle( hSession );

    xResult = prvCheckValidSessionAndModule( pxSession );

    if( ( NULL == RandomData ) ||
        ( ulRandomLen == 0UL ) )
    {
        LogError( ( "Failed to generate random bytes. The buffer to store "
                    "random numbers in was NULL or the length of the buffer was 0." ) );
        xResult = CKR_ARGUMENTS_BAD;
    }

    if( xResult == CKR_OK )
    {
        lMbedTLSResult = mbedtls_ctr_drbg_random( &xP11Context.xMbedDrbgCtx, RandomData, ulRandomLen );

        if( lMbedTLSResult != 0 )
        {
            LogError( ( "Failed to generate random bytes. mbed TLS DRBG failed "
                        "to generate a random number: mbed TLS error = %s : %s.",
                        mbedtlsHighLevelCodeOrDefault( lMbedTLSResult ),
                        mbedtlsLowLevelCodeOrDefault( lMbedTLSResult ) ) );
            xResult = CKR_FUNCTION_FAILED;
        }
        else
        {
            LogDebug( ( "Successfully generated %lu random bytes.", ( unsigned long int ) ulRandomLen ) );
        }
    }

    return xResult;
}
/* @[declare_pkcs11_mbedtls_c_generate_random] */
