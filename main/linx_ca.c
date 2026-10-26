// main/linx_ca.c
// 灵矽语音服务(xrobo*.qiniuapi.com)的信任根:TrustAsia DV TLS RSA CA 2025(中间 CA)。
//
// 为什么直接信任中间 CA,而不是链上的根:
//   该服务的链末张是 "DigiCert Global Root G2" 的**交叉签名**版(由 DigiCert Global Root CA
//   签发),与自签名 G2 同名同公钥。mbedtls 在组链时对同 subject 的证书会判为环路而放弃,
//   因此无论信任自签名 G2 还是 Global Root CA,握手都会以 -0x2700(CERT_VERIFY_FAILED)失败。
//   直接信任签发 leaf 的这张中间 CA,链长为 1,完全不涉及交叉签名,验证稳定。
//
// 维护:这张 CA 到期(见文件末尾日期)或灵矽换 CA 时,需要同步替换本文件。
// 证书是 CA 公开发布的中间证书,不含任何凭证。

#include "linx_ca.h"

const char LINX_TRUSTED_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFnjCCBIagAwIBAgIQCSYyO0lk42hGFRLe8aXVLDANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0yNTAxMDgwMDAwMDBaFw0zNTAxMDcyMzU5NTlaMFsxCzAJBgNVBAYTAkNO\n"
    "MSUwIwYDVQQKExxUcnVzdEFzaWEgVGVjaG5vbG9naWVzLCBJbmMuMSUwIwYDVQQD\n"
    "ExxUcnVzdEFzaWEgRFYgVExTIFJTQSBDQSAyMDI1MIICIjANBgkqhkiG9w0BAQEF\n"
    "AAOCAg8AMIICCgKCAgEA0fuEmuBIsN6ZZVq+gRobMorOGIilTCIfQrxNpR8FUZ9R\n"
    "/GfbiekbiIKphQXEZ7N1uBnn6tXUuZ32zl6jPkZpHzN/Bmgk1BWSIzVc0npMzrWq\n"
    "/hrbk5+KddXJdsNpeG1+Q8lc8uVMBrztnxaPb7Rh7yQCsMrcO4hgVaqLJWkVvEfW\n"
    "ULtoCHQnNaj4IroG6VxQf1oArQ8bPbwpI02lieSahRa78FQuXdoGVeQcrkhtVjZs\n"
    "ON98vq5fPWZX2LFv7e5J6P9IHbzvOl8yyQjv+2/IOwhNSkaXX3bI+//bqF9XW/p7\n"
    "+gsUmHiK5YsvLjmXcvDmoDEGrXMzgX31Zl2nJ+umpRbLjwP8rxYIUsKoEwEdFoto\n"
    "Aid59UEBJyw/GibwXQ5xTyKD/N6C8SFkr1+myOo4oe1UB+YgvRu6qSxIABo5kYdX\n"
    "FodLP4IgoVJdeUFs1Usa6bxYEO6EgMf5lCWt9hGZszvXYZwvyZGq3ogNXM7eKyi2\n"
    "20WzJXYMmi9TYFq2Fa95aZe4wki6YhDhhOO1g0sjITGVaB73G+JOCI9yJhv6+REN\n"
    "D40ZpboUHE8JNgMVWbG1isAMVCXqiADgXtuC+tmJWPEH9cR6OuJLEpwOzPfgAbnn\n"
    "2MRu7Tsdr8jPjTPbD0FxblX1ydW3RG30vwLF5lkTTRkHG9epMgpPMdYP7nY/08MC\n"
    "AwEAAaOCAVYwggFSMBIGA1UdEwEB/wQIMAYBAf8CAQAwHQYDVR0OBBYEFLQSKKW0\n"
    "wB2fKXFpPNkRlkp1aVDAMB8GA1UdIwQYMBaAFE4iVCAYlebjbuYP+vq5Eu0GF485\n"
    "MA4GA1UdDwEB/wQEAwIBhjAdBgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUHAwIw\n"
    "dgYIKwYBBQUHAQEEajBoMCQGCCsGAQUFBzABhhhodHRwOi8vb2NzcC5kaWdpY2Vy\n"
    "dC5jb20wQAYIKwYBBQUHMAKGNGh0dHA6Ly9jYWNlcnRzLmRpZ2ljZXJ0LmNvbS9E\n"
    "aWdpQ2VydEdsb2JhbFJvb3RHMi5jcnQwQgYDVR0fBDswOTA3oDWgM4YxaHR0cDov\n"
    "L2NybDMuZGlnaWNlcnQuY29tL0RpZ2lDZXJ0R2xvYmFsUm9vdEcyLmNybDARBgNV\n"
    "HSAECjAIMAYGBFUdIAAwDQYJKoZIhvcNAQELBQADggEBAJ4a3svh316GY2+Z7EYx\n"
    "mBIsOwjJSnyoEfzx2T699ctLLrvuzS79Mg3pPjxSLlUgyM8UzrFc5tgVU3dZ1sFQ\n"
    "I4RM+ysJdvIAX/7Yx1QbooVdKhkdi9X7QN7yVkjqwM3fY3WfQkRTzhIkM7mYIQbR\n"
    "r+y2Vkju61BLqh7OCRpPMiudjEpP1kEtRyGs2g0aQpEIqKBzxgitCXSayO1hoO6/\n"
    "71ts801OzYlqYW9OQQQ2GCJyFbD6XHDjdpn+bWUxTKWaMY0qedSCbHE3Kl2QEF0C\n"
    "ynZ7SbC03yR+gKZQDeTXrNP1kk5Qhe7jSXgw+nhbspe0q/M1ZcNCz+sPxeOwdCcC\n"
    "gJE=\n"
    "-----END CERTIFICATE-----\n";
