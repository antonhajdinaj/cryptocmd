/*
 * Copyright (c) 2020 Cryptable BV. All rights reserved.
 * (MIT License)
 * Author: "David Tillemans"
 * Date: 08/08/2020
 */
#include <rpc.h>
#include <functional>
#include <vector>
#include <memory>
#include <stdexcept>
#include "CertificateStore.h"
#include "KSException.h"
#include "X509Name.h"

CertificateStore::CertificateStore() : keyStore(MS_KEY_STORAGE_PROVIDER) {
    storeHandle = CertOpenSystemStoreA(NULL, "MY");
    if (storeHandle == nullptr) {
        throw KSException(__func__, __LINE__, GetLastError());
    }
}

CertificateStore::CertificateStore(const std::wstring &keyStoreProvider) : keyStore(keyStoreProvider.c_str()) {
    storeHandle = CertOpenSystemStoreA(NULL, "MY");
    if (storeHandle == nullptr) {
        throw KSException(__func__, __LINE__, GetLastError());
    }
}

std::string CertificateStore::createCertificateRequest(const std::string &subjectName,
                                                       size_t bitLength,
                                                       bool forcePINPasswordProtection) {
    UUID uuid;
    RPC_STATUS status;
    RPC_WSTR   strUuid;
    status = UuidCreate(&uuid);
    if ((status != RPC_S_OK) && (status != RPC_S_UUID_LOCAL_ONLY)) {
        KSException(__func__, __LINE__, (DWORD)status);
    }
    status = UuidToStringW(&uuid, &strUuid);
    auto safeUuid = std::unique_ptr<RPC_WSTR,std::function<void(RPC_WSTR *ptr)>>(&strUuid, RpcStringFreeW );
    if (status != RPC_S_OK) {
        KSException(__func__, __LINE__, (DWORD)status);
    }
    std::wstring stringUuid(reinterpret_cast<const wchar_t *const>(strUuid));
    auto keyPair = keyStore.generateKeyPair(stringUuid, bitLength, forcePINPasswordProtection);

    lastKeyId = stringUuid;

    return createCertificateRequestFromCNG(subjectName, keyPair.get());
}

CertificateStore::~CertificateStore() {
    CertCloseStore(storeHandle, 0);
}

void CertificateStore::importCertificate(const std::string &pemCertificate) {
    DWORD certLg = 0;

    if (pemCertificate.size() > MAXDWORD) {
        throw std::overflow_error("DWORD overflow");
    }
    if (!CryptStringToBinaryA(pemCertificate.c_str(),
                              (DWORD)pemCertificate.size(),
                              CRYPT_STRING_BASE64HEADER,
                              nullptr,
                              &certLg,
                              nullptr,
                              nullptr)) {
        throw KSException(__func__, __LINE__, GetLastError());
    }
    std::vector<unsigned char> cert(certLg);
    CryptStringToBinaryA(pemCertificate.c_str(),
                         (DWORD)pemCertificate.size(),
                         CRYPT_STRING_BASE64HEADER,
                         cert.data(),
                         &certLg,
                         nullptr,
                         nullptr);

    // TODO: maybe allow other stores then 'MY'
    PCCERT_CONTEXT certContext = nullptr;
    if (!CertAddEncodedCertificateToStore(storeHandle,
                                          X509_ASN_ENCODING,
                                          cert.data(),
                                          certLg,
                                          CERT_STORE_ADD_ALWAYS,
                                          &certContext)) {
        CertCloseStore(storeHandle, 0);
        throw KSException(__func__, __LINE__, GetLastError());
    }
    auto keyPair = keyStore.getKeyPair(certContext->pCertInfo->SubjectPublicKeyInfo);
    if (keyPair != nullptr) {
        CRYPT_KEY_PROV_INFO cryptKeyProvInfo = {
                const_cast<LPWSTR>(keyPair->getName().c_str()),
                const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER),
                0,
                0,
                0,
                nullptr,
                AT_SIGNATURE
        };
        if (!CertSetCertificateContextProperty(certContext,
                                               CERT_KEY_PROV_INFO_PROP_ID,
                                               0,
                                               &cryptKeyProvInfo)) {
            CertCloseStore(storeHandle, 0);
            throw KSException(__func__, __LINE__, GetLastError());
        }
        DWORD keyHandle = (DWORD)keyPair->getHandle();
        if (!CertSetCertificateContextProperty(certContext,
                                               CERT_NCRYPT_KEY_HANDLE_PROP_ID,
                                               0,
                                               &keyHandle)) {
            throw KSException(__func__, __LINE__, GetLastError());
        }
    }
}

std::string CertificateStore::createCertificateRequestFromCNG(const std::string &subjectName, KeyPair *keyPair) {
    // reference: https://docs.microsoft.com/en-us/windows/win32/seccrypto/example-c-program-making-a-certificate-request

    try {
        X509Name subject(subjectName);

        CERT_REQUEST_INFO  CertReqInfo;
        CertReqInfo.dwVersion = CERT_REQUEST_V1;
        CertReqInfo.Subject = subject.getEncodedBlob();
        CertReqInfo.SubjectPublicKeyInfo = *(keyPair->getPublicKeyInfo());
        CertReqInfo.cAttribute = 0;
        CertReqInfo.rgAttribute = nullptr;

        DWORD certSignReqLg = 0;
        CRYPT_OBJID_BLOB  Parameters;
        CRYPT_ALGORITHM_IDENTIFIER  SigAlgo;
        memset(&Parameters, 0, sizeof(Parameters));
        SigAlgo.pszObjId = szOID_RSA_SHA256RSA;
        SigAlgo.Parameters = Parameters;
        if (!CryptSignAndEncodeCertificate(keyPair->getHandle(),
                                           0,
                                           X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                           X509_CERT_REQUEST_TO_BE_SIGNED,
                                           &CertReqInfo,
                                           &SigAlgo,
                                           nullptr,
                                           nullptr,
                                           &certSignReqLg)) {
            throw KSException(__func__, __LINE__, GetLastError());
        }
        auto certSignReq = std::unique_ptr<BYTE[]>(new BYTE[certSignReqLg]);
        if (!CryptSignAndEncodeCertificate(keyPair->getHandle(),
                                           0,
                                           X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                           X509_CERT_REQUEST_TO_BE_SIGNED,
                                           &CertReqInfo,
                                           &SigAlgo,
                                           nullptr,
                                           certSignReq.get(),
                                           &certSignReqLg)) {
            throw KSException(__func__, __LINE__, GetLastError());
        }
        DWORD b64CertReqLg = 0;
        if (!CryptBinaryToStringA(certSignReq.get(),
                                  certSignReqLg,
                                  CRYPT_STRING_BASE64REQUESTHEADER,
                                  nullptr,
                                  &b64CertReqLg)) {
            throw KSException(__func__, __LINE__, GetLastError());
        }
        auto b64CertReq = std::unique_ptr<char[]>(new char[b64CertReqLg]);
        CryptBinaryToStringA(certSignReq.get(),
                             certSignReqLg,
                             CRYPT_STRING_BASE64REQUESTHEADER,
                             b64CertReq.get(),
                             &b64CertReqLg);
        return std::string(b64CertReq.get(), b64CertReqLg);
    }
    catch (KSException &e) {
        keyStore.deleteKeyPair(keyPair->getName());
        throw e;
    }
}

std::string CertificateStore::pfxExport(const std::string &issuer,
                                        const std::string &serial,
                                        const std::wstring &password) {
    DWORD issuerNameLg = 0;

    CERT_INFO certificateInfo{0x00};
    DWORD serialNumberLg;
    std::string tmpSerial(serial);
    if ((tmpSerial.rfind("0x", 0) == 0) ||
        (tmpSerial.rfind("0X", 0) == 0)) {
        tmpSerial.erase(0,2);
        if (tmpSerial.size() % 2) {
            tmpSerial.insert(0,1,'0');
        }
    }
    if (!CryptStringToBinaryA(tmpSerial.c_str(),
                              tmpSerial.size(),
                              CRYPT_STRING_HEX,
                              nullptr,
                              &serialNumberLg,
                              nullptr,
                              nullptr)) {
        throw KSException(__func__, __LINE__, GetLastError());
    }
    auto serialNumber = std::unique_ptr<BYTE[]>(new BYTE[serialNumberLg]);
    CryptStringToBinaryA(tmpSerial.c_str(),
                         tmpSerial.size(),
                         CRYPT_STRING_HEX,
                         serialNumber.get(),
                         &serialNumberLg,
                         nullptr,
                         nullptr);
    std::vector<BYTE> reversedSerNum(serialNumberLg);
    for (int i=0; i<serialNumberLg; i++) {
        reversedSerNum[serialNumberLg - 1 - i] = serialNumber[i];
    }
    certificateInfo.SerialNumber.cbData = serialNumberLg;
    certificateInfo.SerialNumber.pbData = reversedSerNum.data();
    // UTF8 Version test
    X509Name issuerName(issuer);
    certificateInfo.Issuer.cbData = issuerName.getEncodedBlob().cbData;
    certificateInfo.Issuer.pbData = issuerName.getEncodedBlob().pbData;

    PCCERT_CONTEXT certificateCtx = CertGetSubjectCertificateFromStore(
            storeHandle,
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            &certificateInfo);
    if (certificateCtx == NULL) {
        // PrintableName Version test
        X509Name issuerName(issuer, false);
        certificateInfo.Issuer.cbData = issuerName.getEncodedBlob().cbData;
        certificateInfo.Issuer.pbData = issuerName.getEncodedBlob().pbData;
        certificateCtx = CertGetSubjectCertificateFromStore(
                storeHandle,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                &certificateInfo);
        if (certificateCtx == nullptr) {
            throw KSException(__func__, __LINE__, GetLastError());
        }
    }

    HCERTSTORE pfxStore = CertOpenStore (CERT_STORE_PROV_MEMORY,
                                         0,
                                         0,
                                         0,
                                         nullptr);
    if (pfxStore == nullptr) {
        throw KSException(__func__, __LINE__, GetLastError());
    }
    if (!CertAddCertificateContextToStore(pfxStore,
                                          certificateCtx,
                                          CERT_STORE_ADD_USE_EXISTING,
                                          nullptr)) {
        CertCloseStore(pfxStore, 0);
        throw KSException(__func__, __LINE__, GetLastError());
    }
    CRYPT_DATA_BLOB pfxData = {0, nullptr };
    if (!PFXExportCertStore(pfxStore,
                            &pfxData,
                            password.c_str(),
                            EXPORT_PRIVATE_KEYS)) {
        CertCloseStore(pfxStore, 0);
        throw KSException(__func__, __LINE__, GetLastError());
    }
    auto pfxDataBuf = std::unique_ptr<BYTE[]>(new BYTE[pfxData.cbData]);
    pfxData.pbData = pfxDataBuf.get();
    if (!PFXExportCertStore(pfxStore,
                            &pfxData,
                            password.c_str(),
                            EXPORT_PRIVATE_KEYS)) {
        CertCloseStore(pfxStore, 0);
        throw KSException(__func__, __LINE__, GetLastError());
    }
    // TODO: replace with Base64Utils
    DWORD base64PfxDataLg = 0;
    if (!CryptBinaryToStringA(pfxData.pbData,
                              pfxData.cbData,
                              CRYPT_STRING_BASE64,
                              nullptr,
                              &base64PfxDataLg)) {
        CertCloseStore(pfxStore, 0);
        throw KSException(__func__, __LINE__, GetLastError());
    }
    auto base64PfxData = std::unique_ptr<char[]>(new char[base64PfxDataLg]);
    CryptBinaryToStringA(pfxData.pbData,
                         pfxData.cbData,
                         CRYPT_STRING_BASE64,
                         base64PfxData.get(),
                         &base64PfxDataLg);
    return std::string(base64PfxData.get(), base64PfxDataLg);
}

bool CertificateStore::isCACertificate(PCCERT_CONTEXT certificateCtx)
{
    for (int i = 0; i < certificateCtx->pCertInfo->cExtension; i++)
    {
        if (strncmp(certificateCtx->pCertInfo->rgExtension[i].pszObjId, szOID_BASIC_CONSTRAINTS2, strlen(szOID_BASIC_CONSTRAINTS2)) == 0)
        {
            CERT_BASIC_CONSTRAINTS2_INFO info = { 0 };
            DWORD size = sizeof(CERT_BASIC_CONSTRAINTS2_INFO);
            CryptDecodeObject(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                              szOID_BASIC_CONSTRAINTS2,
                              certificateCtx->pCertInfo->rgExtension[i].Value.pbData,
                              certificateCtx->pCertInfo->rgExtension[i].Value.cbData,
                              0,
                              &info,
                              &size);
            return (info.fCA == TRUE);
        }
    }
    return false;
}

void CertificateStore::pfxImport(const std::string &pfxInBase64,
                                 const std::wstring &password,
                                 bool forcePINPasswordProtection) {
    DWORD pfxLg = 0;
    if (!CryptStringToBinaryA(pfxInBase64.c_str(),
                              pfxInBase64.size(),
                              CRYPT_STRING_BASE64_ANY,
                              nullptr,
                              &pfxLg,
                              nullptr,
                              nullptr)) {
        throw KSException(__func__, __LINE__, GetLastError());
    }
    auto b64CertReq = std::unique_ptr<unsigned char[]>(new unsigned char[pfxLg]);
    CryptStringToBinaryA(pfxInBase64.c_str(),
                         pfxInBase64.size(),
                         CRYPT_STRING_BASE64_ANY,
                         b64CertReq.get(),
                         &pfxLg,
                         nullptr,
                         nullptr);
    CRYPT_DATA_BLOB cryptDataBlob {
        pfxLg,
        b64CertReq.get()
    };
    DWORD dwFlags = CRYPT_EXPORTABLE | CRYPT_USER_KEYSET | PKCS12_ALWAYS_CNG_KSP;
    if (forcePINPasswordProtection) {
        dwFlags = dwFlags | CRYPT_USER_PROTECTED;
    }
    HCERTSTORE pfxStore = PFXImportCertStore(&cryptDataBlob,
                                             password.c_str(),
                                             dwFlags);
    if (pfxStore == 0) {
        throw KSException(__func__, __LINE__, GetLastError());
    }
    PCCERT_CONTEXT certificateCtx = nullptr;
    while ( (certificateCtx = CertEnumCertificatesInStore(pfxStore, certificateCtx)) != nullptr )
    {
        if (isCACertificate(certificateCtx)) // Don't import CA Certificates
            continue;
        if (!CertAddCertificateContextToStore(storeHandle,
                                              certificateCtx,
                                              CERT_STORE_ADD_REPLACE_EXISTING,
                                              0)) {
            CertCloseStore(pfxStore, 0);
            throw KSException(__func__, __LINE__, GetLastError());
        }
    }
    CertCloseStore(pfxStore, 0);
}

const std::wstring &CertificateStore::getLastKeyId() {
    return lastKeyId;
}

void CertificateStore::forcePasswordPINProtection(strongKeyProtection k) {
    HKEY  hRegKeyHandle = NULL;
    DWORD dwValue       = static_cast<DWORD>(k);
    DWORD dwValueSize   = sizeof(dwValue);
    DWORD dwType        = REG_DWORD;
    int   iRet;

    /* Try to open this key */
    iRet = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Policies\\Microsoft\\Cryptography",
                          0,
                          NULL,
                          REG_OPTION_NON_VOLATILE,
                          KEY_READ,
                          NULL,
                          &hRegKeyHandle,
                          NULL);
    if ( iRet == ERROR_SUCCESS )
    {
        iRet = RegQueryValueEx (hRegKeyHandle,
                                "ForceKeyProtection",
                                NULL,
                                &dwType,
                                (LPBYTE) &dwValue,
                                &dwValueSize);
        if ( ( iRet    == ERROR_SUCCESS ) &&
             ( dwValue == 2             ) )
        {
            /* We are finished */
            RegCloseKey(hRegKeyHandle);
            return;
        }
    }

    RegCloseKey(hRegKeyHandle);
    hRegKeyHandle = NULL;

    iRet = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Policies\\Microsoft\\Cryptography",
                          0,
                          NULL,
                          REG_OPTION_NON_VOLATILE,
                          KEY_READ | KEY_WRITE,
                          NULL,
                          &hRegKeyHandle,
                          NULL);
    if ( iRet == ERROR_SUCCESS ) {
        dwValue = 2;
        iRet = RegSetValueEx (hRegKeyHandle,
                              "ForceKeyProtection",
                              0,
                              REG_DWORD,
                              (LPBYTE) &dwValue,
                              sizeof(dwValue));
        if ( iRet != ERROR_SUCCESS ) {
            RegCloseKey(hRegKeyHandle);
            throw KSException(__func__, __LINE__, GetLastError());
        }
    }
    else
    {
        /* Failed creating registry key */
        RegCloseKey(hRegKeyHandle);
        throw KSException(__func__, __LINE__, GetLastError());
    }

    RegCloseKey(hRegKeyHandle);
}