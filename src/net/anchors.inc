/*
        Trust anchors: the Mozilla TLS server roots this client can verify.

        Generated on 2026-09-14 from Arch ca-certificates-mozilla 3.127
        (/etc/ssl/certs/ca-certificates.crt, 121 roots).  Left out, for a key this
        client does not verify: e-Szigno TLS Root CA 2023.  An entry is
            first 8 bytes of SHA-256 over the root certificate's subject
            Name DER, first 8 bytes of SHA-256 over its key bytes,
            key type (1 P-256, 2 P-384, 3 RSA), RSA public exponent,
            key length, key in base64
        where the key bytes are x || y for a curve and the unsigned
        big-endian modulus for RSA.  The hashes only find candidates: tls.c
        compares the whole key, or verifies a signature with it, before it
        trusts anything.  To regenerate, walk the bundle with python
        cryptography and print these fields for every RSA root of 2048 to
        4096 bits with an odd exponent and every P-256 or P-384 root, sorted
        by common name; the comment above each entry is that name and the
        first 16 hex digits of the certificate's SHA-256.
*/

    /* AC RAIZ FNMT-RCM, sha256 ebc5570c29018c4d */
    {{0x7a, 0x5a, 0xac, 0x0a, 0x43, 0x18, 0x82, 0x2b},
     {0x68, 0x70, 0xa9, 0x20, 0xfd, 0x11, 0x5a, 0x8d},
     3, 65537, 512,
     "unGAekyGbn/IE23Axn0cAJePLAwjuxCaQKkat4eI+JtWavvme46Lko6nJV1ZEds2"
     "LrdRFx+pCB8EFyRYqjdKGN/lOdRX/dfBLJEBkeIi1APAWPx3R+yPPnRDuqw0jU04"
     "dmeOsMhvMDNYcVy09Wtu1AFQuBN+bEqjSdEgGe68wCkYZafe/u/dCpAh5xpnkkIQ"
     "mF9PMLw+HEW0ENdoQBTAQPrndxd65guPZVs82ZpS27W9nkbPPeuRBQLAlrJ2TE0Q"
     "ljuS+px/D5nfviM1RR4CXP61qJuZJdpe8yLDOfXkKi7Txh/EbKrFHGoBBUov0sXB"
     "qDQmXWal0gIh+Ri3BvVOmW+oq0xR6M9QGMV3yDkJLEmSMpmouxcXebBaxeajxFll"
     "RzWDXqnoNQuZu+TNIMabSgY5tWj8IrruVYwrTurzseP8tpma1UL6cU0Iz4ceanF9"
     "+dO06aVxgXvCTkeWpfZ2haMoj+mAboFTpW1fuEj5wvk2pi5J/7iWwowHs5uIWPzr"
     "GxzeLXDil5IwoYnjvFWoJ9ZL7ZCti/pjJVktqDXdypczvOXNx53R7O9eDkqQBiZj"
     "rbnZNS0HunZlLKxXj330B5TXgQKWXaMHSdV60Ff5G+dTRnWqsHlCy2hxCOlgvTlp"
     "zvSvw1ZAx61Sognkb4ZHih/rKCddgyCvBMlsVpqLRvU="},
    /* AC RAIZ FNMT-RCM SERVIDORES SEGUROS, sha256 554153b13d2cf9dd */
    {{0xd5, 0x75, 0xc0, 0x05, 0x50, 0xd9, 0x3a, 0x3c},
     {0x74, 0x63, 0x7e, 0x5b, 0xfb, 0x52, 0xa3, 0x71},
     2, 0, 96,
     "9rpXU8jKq982SlIh5JfSg2ee8GVR0F6Hx0exWfJXR5sAApNEF2nbQsexsjoYDrRd"
     "jLNmXaE0+TYsSdvzRvyzRGlEE2b918X9rzZNzgNNB3HPr2oF0qJDWgpSbwEDTo6L"},
    /* ACCVRAIZ1, sha256 9a6ec012e1a7da9d */
    {{0x3e, 0xd7, 0xcd, 0xa3, 0x0a, 0x8f, 0xd9, 0xef},
     {0xcb, 0xb6, 0xa9, 0xe8, 0xe9, 0x8b, 0xf9, 0x60},
     3, 65537, 512,
     "m6mrv2FKl68vl2aadF/Q2Zb9z+LkZu8fH0czwkSj35reH7VU3RV8aTURb7vIDI5q"
     "GB7Yj9kWvBBINlzwY7OQWlwkN9ej1ssJcbnxAXKEsH3bTYDN/NNvyfjatg6C0kWF"
     "qBtoqD3o9ERsvaHCywO+jD4TAITfSkjA4yIK6Ok3pxhMsQkNI1Z/BE3ZF4QYpcja"
     "QJRz684OVzwDgTqdCqFXQ2msV215kHjltbQ72LxMjSihp6OnugJOJdEqru2uAyK4"
     "ayAPMChUlX/g7s4KZp3RQC1uIq+dGsEFGdJvwPKf+HuzAkL7UKkdLZMPI6vGwQ+S"
     "/9CiFfVTCXEc/0UThOYmXvjgiBwK/Ba2qHMGuPBjhAKgxlrs53TfcK6jgyXq1seX"
     "h5OnxoqKM5dgNxA+lz5uKRXWoQ/RiCwSn2+qpMZC60Gi45VD0wGFbY67O/MjNsf+"
     "O+ChJQdIq8mJdP8Ij4C/wJZl8+7sS2i9nYjDMbNA8ejP9ji7nOTRf9TlWJt8+tTz"
     "Dpt1keS6Ui4ZftH1zVoZ/LoG9vtSqEuZBN34+bSLUKNOYonwhyT6g0LBh/rVLSkq"
     "WnF6ZGrXJ2BjDdvOSfWNH5CJMhf4c0O40lqThmHW4XUK6nlmdohPcesEJdYKWnqT"
     "5blLF0APsba59d5P3OCzrDsRcGCESkNumSDAKXEKwGU="},
    /* Actalis Authentication Root CA, sha256 55926084ec963a64 */
    {{0x3c, 0xa5, 0x68, 0x79, 0x3c, 0xb9, 0xb2, 0xc5},
     {0xb6, 0x51, 0x8e, 0xb1, 0xf7, 0x2b, 0xf5, 0x7a},
     3, 65537, 512,
     "p8bEpSmkLO/lGMWwUKNvUTufClrJwkg4CsIcoBh/kbWHuUA/3R1oHwiD1S0eiKD4"
     "j1aPbZkCkpAW1V8IbInX4ay8IMKx4INRimlNAJZaby/ARH6jDuSRzVju3PvHHkVH"
     "3Se5CAGfpiEd9UEtL0z9KK3giq0itFZljoZUj5NDKd45RnijMCO6zfB9E1fAXdKD"
     "a0hMxKufgFpbOr3JpyI/gCczWw63igxdBzcIy2zSekciRDXFzMwujt0q7bd9Zg1f"
     "YVEiVRvjRuPjPdA1YprbrxTIW6HMiRvhMCb8oJsfgadHHwTrozmSBp+Z07/T6k9Q"
     "nBn+locePGX2oxgkg4YQ51Q+qDp2JE+BIcXjDwL4k5RHILv+1A7TaLndxHqEguNT"
     "VHnd25zS8gebLra8Pu2Fbe8lEfKXGkJh90qX6IuxEAf6ZYGyojnP9zz/GPvG8VqL"
     "WeICrHuS0E4UT1lF9gxeKF+w6D9Fz8+vm2/7hNN3WpVvrJSEnu68wEqPSpP4RCHi"
     "MUVhUE4Q2OM1fEwZtN4Fv6MGn8i1zeQf1xcGDXqVdFUNaBr8EBtiZJ1t4JWgw5QH"
     "Vw0U5r0F+7if5t+L4sbnfpb2U8WANFAoWPASUHEXMLrmeGO89LKtmyuy/uE5jF66"
     "CyCU3nuDuP/jVo23Eek7jPKxwV2dpAtMK9myGPW1n0s="},
    /* Amazon Root CA 1, sha256 8ecde6884f3d87b1 */
    {{0x7a, 0xfc, 0x26, 0x5b, 0x7b, 0x42, 0x8e, 0x05},
     {0xfa, 0xff, 0x5c, 0xb2, 0x07, 0x69, 0x79, 0xbb},
     3, 65537, 256,
     "sniAccp41eNxr0eAUHR9btjXiHb0mWj3WCFg+XSEAS+sAi2G06BDek6ypNA2ugG+"
     "jdtIyAcXNkz07ogjxz7rN/W1GfhJaLDe17l2OB1hnqT+gjal5UpW5EXh+f20Fvp0"
     "2pybNTkv+rAgUAZsetCAsqb5r+xHGY9QOAfcooc5WPi61an5SGcwlu6UeF5viaNR"
     "wDCGZqFFZrpU66PDkflI3P/R6DAtfS10cDXXiCT3nsRZbrtzhxfyMkYouEP6tx2q"
     "yrTynyQOLUv3cVxeaf/qlQLLOIquUDhv2/stYhvFxx5U4XfgZ8gPnIcj1j9AIH8g"
     "gMSATD47JCaOBK5smsiqDQ=="},
    /* Amazon Root CA 2, sha256 1ba5b2aa8c65401a */
    {{0xdd, 0x78, 0x02, 0xa0, 0x0e, 0x09, 0xce, 0x4d},
     {0x17, 0xbc, 0x4c, 0x06, 0x4c, 0xeb, 0x64, 0x23},
     3, 65537, 512,
     "rZafLZxKTEqBeVGZ7IrLa2BRE7xNbQb8sAiN3RkQascmDDXYwG8ghOmUsZuFA8Nb"
     "20royPiQdtlbT+NM6AY2TcyarD0MkCuS1AYZYKw3RHmFgYKtWjfgDcydpkxSdupD"
     "nbcE0VD2VeDV0qZJhek36cp+rlyVTUiaP64gWm2Ildk0uFIaQ5Cwv2wFubZ4t+rQ"
     "5Do8ElNi/0rye741BakSNOPzZHRiLD0ASVoo/jJEu4fdZScCcTvaSvcf2s33IVWQ"
     "Tw/sroLhn2vZRdO78F+H7TwsOYbaP97sclXreaOt2918sLoczvzeTzV2zw/4eB9q"
     "NlFGJ2Fb6Z7P8KJVfXwlim8vtMXPhC4r/Q1REGz7Xxu8G37FrjuYATGS/wtX9Jqy"
     "uVfpq+8NdtHw7vTOhqfgbum0aaHfafYzxmkulxOepYewVxCBN8lTs7t/9pLRnNAY"
     "9JJu2oNPpmOZTKX7Xu8hZHogX2xkhRXLN+liDAsqFtwBLjLaPkv1njr2F0CU756R"
     "CIb6vmOoWjPsy3RDlflsaVI2xylv/FUDXB/7n71H6+dJR5ULTokiCUng9WEe8b8u"
     "inJugFn/Vzr5dTKjTl/s7Shi2U1z8syBF2Dtzevc26fKxX4CvfJUCFT9tC0JLBdU"
     "SpjRVOFRZwjS7W5+bz/SLYFZKWbLkDmVER50J/7d668="},
    /* Amazon Root CA 3, sha256 18ce6cfe7bf14e60 */
    {{0xe2, 0x44, 0xa8, 0xce, 0x86, 0xd7, 0xc8, 0xa5},
     {0xdd, 0xbc, 0x46, 0xde, 0xf9, 0x13, 0xca, 0x1e},
     1, 0, 64,
     "KZenxkF/wA2b6AEbVsbyUqW6LbIS6NIu1/rJxdiqbR9zgTs7mGs5fDOlxU6GjoAX"
     "aGJFV31EWB2zN+VnCOtm3g=="},
    /* Amazon Root CA 4, sha256 e35d28419ed02025 */
    {{0xed, 0x63, 0xc1, 0xcd, 0xb7, 0xf3, 0xb9, 0xc5},
     {0x03, 0xcf, 0x1e, 0x8b, 0xab, 0x5f, 0xa0, 0x4a},
     2, 0, 96,
     "0quKN0+jUw3+wYp7S6h7RktjsGL2LRvbCHEh0gDoY72aJ/vwOW5d6j2lyYGqo1sg"
     "mEVdFtv96BBt45zg471fhGLzcGQzoMskL3C6iKEqoHX4ga5iBsSB2zluKbAe+i5c"},
    /* ANF Secure Server Root CA, sha256 fb8fec759169b910 */
    {{0x78, 0xc7, 0xc3, 0x0a, 0x81, 0xe8, 0x96, 0x6e},
     {0x66, 0x14, 0x06, 0x00, 0x37, 0xb4, 0xb2, 0x8c},
     3, 65537, 512,
     "2+trK+ZkVJWCkKNypBkBnZwLgV9zSbqnrPMETnuWC+wR4FumHM4b0g2DHCu4nh1+"
     "RTJgDwfpd1h+n2rIYU62JsFMjf9M7zSyH2XYuXj1ralxue9PWB2l3nQgl6HtaEze"
     "khdLvKv/ZZqe+0fZV3LzCaGudkQTbpwtRDm8+cc7pFg9Qb20wkmjyA3Sly8HZVIA"
     "p27Ir2js9BSWtlcfVsM5nytt5PM+9jVk2gwcoYRLL0tL4iwknW2TQOu1I44yym9F"
     "06iJex7PHvpbQ4vNzagPasoMXrmeR4/w2bYKC1hlFzO5I+R3GX3LSi6Se08vEHex"
     "jS9onGLM4FD47JGnVExXCdV2Y8XoZR7ubWrPCZ36fE+tYAj9VpkPFSx7qYCrjGGP"
     "Sgd2Qt499N2yJDNbuLWjRMmsf3c8HSPsgqmm4sgGTAL+rFyZmQsvEIqm9H/Vh3QN"
     "WUlF9vBxXDkp1r9KI4v1XwFj0odzKLVLCvX4q4IsfnMlMh0LYwoXgQD/tnZe57Sx"
     "QMohu9WAUeVIUmcs0mGJBw0PzkJ3wERznERQoNsQCi2VHIGv5BzlFB7xNkEBAi99"
     "c6feQsxM6YkNVvefkdQDxmzJj9vYHOBAmF1mmZiAbi3/AcXOy0YfrALGQ+auooQ8"
     "xU4ePW3JFEzjLkG7yjm/NjwqGapBh06lzksyed2QSX8="},
    /* Atos TrustedRoot Root CA ECC TLS 2021, sha256 b2fae53e14ccd7ab */
    {{0x56, 0x1c, 0xbe, 0xce, 0xde, 0x82, 0x9c, 0xa0},
     {0x46, 0x42, 0x7d, 0x2b, 0x5f, 0x7c, 0x88, 0x28},
     2, 0, 96,
     "loZYKDcKZ9Cg3iQZGeHkBQcfl+3oZIK59sRxUM6KDP/XtXa7oWyTbIOiaG6l2b4s"
     "iJVBzV3dscqDY4PMwL502eCdpO5KTlbgmClBk1IQ1SQ4AjJn8ZQSb+/Xxd4u/RmA"},
    /* Atos TrustedRoot Root CA RSA TLS 2021, sha256 81a9088ea59fb364 */
    {{0x76, 0xcf, 0xb1, 0x43, 0x72, 0xe6, 0x4f, 0x28},
     {0x85, 0xe2, 0x4a, 0x10, 0xd9, 0xe0, 0xa3, 0x67},
     3, 65537, 512,
     "toAOxHm9BYx9sKOdTSJNy/BBl01Z4NH+VoyX8te9j2y3I49f1cTYQcvyAh5x5en2"
     "XssIKl4w8i1mx4QbZFc4nXUtVsYvYe+W/CBGvevUez8/fEc4BKkbqlLfEzfTFRVO"
     "vV98r61jx3ncCHvVoOX3W3WsgFWZkmGbzSoXfduP9LVq6hdKZChmFSlsAvFr1bqj"
     "M9xaZ6cF4r9lthawEO3NUDPJcFDsGY6wx/J0W2tExn2WuZgIWWbeKQGb9Cpt0xU6"
     "kGpn8bRrZtkh68rZYnxGEFzedUlnnkL5/nWpo63/dgpnQOPF943HhZpZnmKaau1F"
     "h5hnstVKPNe0OwANwI8f4UDErmwh3El+fsqyjW22v5MvoVw+j8rtgI5Y4dtXz4U2"
     "OLJxpAmMkokIiEjxQGMYsluMWuPD0xeqqxmjLBvk1cbiZnrXghmmOxYsL3GHX0We"
     "lXOTwkKBIROW1527k2gV+p2kHYzygeBYBr3JtuP2iV2J+axEoctr+hbxx1A9JNr3"
     "w+SH1VbxT5Aw+kUJWdo0zuATHAR8ANSbhqRAvNncTFd+rrcztl524WWLZt+NyteY"
     "r842mIycg5kDcPOvdO3GDjbnvezBc6eUWsuSZIKmAMFwoW4sKeFYV+xafJlrJaSQ"
     "OoD0IJ2azsct+bJLKZWD6TWNp0lIpw9MGZHQ9b8Q4HE="},
    /* Autoridad de Certificacion Firmaprofesional CIF A62634068, sha256 57de0583efd2b26e */
    {{0xe5, 0xb3, 0xf8, 0x01, 0x80, 0x24, 0x64, 0x1e},
     {0x67, 0x64, 0x4d, 0xc3, 0x19, 0xd9, 0x38, 0x80},
     3, 65537, 512,
     "ypZrjur4+/GiNeB/TNrgw1LXfbYQyAJes0MqxE9qssocXSiaeBEaaVlXr7UgQuSL"
     "D+bfW6YDki/1EeRi1zJxONkEDHGrPVF+DwffYwVc6b+Ub8EpgsC02lGwwTy7rTdK"
     "XMrxSzYOJKu/w4R3/ahQ9LHnxi/SLVmNegpOlmlSAqo2mOz8+hSDDDcfyZI3f9eB"
     "LeXEueA+NP5n9D5m0dP0QM9eYjQPcAY+IBhazvdyGyVsk3QUk6NzsQ6qhxAjWV8g"
     "BRlH7WiOkhLKXfzWK7KSPCDP4V+vIL6gdn925ewahmEzPud7tD+gD46iuWpvuYcm"
     "b0FsiKZQ/WpjC/WTFhsZj7Ltm5vJkPUBDN8ZPQ8+OCPJL48M0QL+G1XWTtCNPK9P"
     "pPP+ryrTBZ15CKHLVzG0nMiQsmf0GBaTOvxH2NF4ljEfuisMX12ZrWOJWiQgdtjf"
     "/atOpiKqnV7mJ4p9aCmj54q42hG7Fy2ZnRMkRvfF4tifjn/Hj3RtWrLocvWs7iQQ"
     "rS8U2v8tmkZxR75C37sB2/R/0yiPMVlb08kCprRSym6X+0PFCCZvivS7/Z8oqg3V"
     "RfMTOh3YwHiPQWc8HpRkrnsLxejZAYg5GpeGZEHVO4cMbvoPxr1IFL85TdSeQbaP"
     "lh1jlpPZlQZ4MWieNwY7gIlFYTkjxxtEoxXlHPiSMLs="},
    /* BJCA Global Root CA1, sha256 f3896f88fe7c0a88 */
    {{0xf5, 0xd3, 0x14, 0x60, 0x2b, 0xda, 0x6a, 0x66},
     {0x8b, 0xde, 0xcd, 0x3a, 0x5e, 0x1c, 0x1f, 0x93},
     3, 65537, 512,
     "8WYIvdnFFWHLhARBpWk3dx3BsHv6w3dIkBNyZNG4fJA1nRh5iOOXATxHgfIOopgN"
     "nj834BmykPJGHJKxOmHO+rdGngOG1zNu7fdFjHY33m6WkffXfiuHF9WLNe6EkXJX"
     "3GDDw7nnx2ckI09jCmP2Zn1LVac/eGRJaRKX4EwN0wmgMjA6+p/A8pzFEiouHLUE"
     "M9qkOBFq3sYY9kc6IkGHIvzEiShU2IylMAr4FxbKrDf9eaeRF3g4ma1Y7bLezIl9"
     "A5yziWXn4zuxIoaPBm14B/2REn+waxyJDfm4y3RbB8LI9DXRZGN66W6aKNYwveYb"
     "3RWvhOqcx8r1DuryXSmHj2lzOb4uJG9FIazF1GklBoOtekiFEywNBrhseVb8o2cy"
     "gfVXpcpXQmnpXCRh7+IwGE5EmFVvesKT2Bm23nxHihFOSUfbKJQCC5RKLPkS0E/o"
     "MX5ser+mP5s5PQIWoxizZ6xbPyyDK2c5gVy5fpTVZN2ej26u6HxbtNdqR0jXfrPU"
     "LY5Wdk7PafFuRGzUJOqNJKEYv71X/qmZNbXbEHe4PUi61sHn8SM+19+FnSc81EC9"
     "Cgy99eeNJdaBdIdG1Cl1okJs93OJ532/ekof0yLJFVXP3298VdCkiwcRN1+DpiZX"
     "pgFbfv5YaAep6XrZuej/UB+rwrTAzujq/Q+9jU24vHE="},
    /* BJCA Global Root CA2, sha256 574df6931e278039 */
    {{0xd2, 0xa9, 0xaf, 0x31, 0xab, 0x91, 0xef, 0x7f},
     {0x98, 0x83, 0xbc, 0xa8, 0x80, 0xd4, 0x33, 0xc2},
     2, 0, 96,
     "ncuAkY1TZ7W5ULED+OVJH0EiCbBRUljWKzSPxRJGFMWLLyyE/yxuqNXxCeMDIRTE"
     "Qz18wSzES2pKzemH4H32Ir76SlG4MIr94d4YEgr2R7fnF78nitRBTJY8YJbB/RUc"},
    /* Buypass Class 2 Root CA, sha256 9a114025197c5bb9 */
    {{0x3a, 0xf7, 0xf6, 0xd7, 0x42, 0x95, 0x1a, 0x67},
     {0x48, 0x3b, 0x2f, 0xdc, 0xf2, 0xac, 0xda, 0x28},
     3, 65537, 512,
     "18de98EH1Hf7QyH09PVp5O4yAdujhh/kWQ2653WDUuvqHGEVSLsdB8qMrrDclp3q"
     "w2CShoIoc5xWBv9LZPAMKjdJteXPDHzu8Uq7czBl89Uvg7Z+4+f1nqtg+dPxnZJ0"
     "iuQclqxbgOm19DGHo1H8x36hb45Td9SXwVUzkj4YL3XUrYZJy5WvVAZs2AYTjVv/"
     "4SYZWcAkuoFxeZBEUGgklF+4sxHxKUFho0HLIzbVwfEyUBBOf/SGk+yE0468S79c"
     "AU4HPdwUipQKpOpz+wtR6BMHGPoO8SvRVBV9POH3tBlCZ2Jed+CiVey22WkX1Tqv"
     "RO1KxZ7keid85XXXqssl599rCtsPTZNOqKDNey7yWQFqtw24B4F+izgbOOYKV5k9"
     "7iHoo/UMFt2L7DSOnCocABUXjWiD0nCfGAjNEWjVyWtSzcRGj9y189hXcx7plDkE"
     "v9PeON60U+xpHKJ+xI/kG3Ct8qL5+/cWZGZpn0lRouIVGGcGSn/VbLVNszPgYetd"
     "vumYDzLXHUs8LloBUpEJ8t/qjdgGQGOqEeT+wzeeFFI/9OLM8mGT0f1na9dSrr9o"
     "q0BDoFc1U3jwU/hhQgdkxtdvm0w4DWOsYq82i6JzCg31Ib10qk3qcgNJ28dfHWJj"
     "x/3dkewz7vVttG4waN7I1iawdV57tAcgmKF2MrhNbE8="},
    /* Buypass Class 3 Root CA, sha256 edf7ebbca27a2a38 */
    {{0x1f, 0xae, 0x3f, 0x05, 0x84, 0x8a, 0x69, 0x06},
     {0x89, 0xfd, 0x8c, 0xd4, 0xf9, 0xca, 0x03, 0x59},
     3, 65537, 512,
     "pdoKlRZQ45XyXp12MQYyepvxEHa4AJq1UjbNJEewnxhkvJr2+tV52JBiTCIv3jg9"
     "1uCo6Rws23gR6Y5oURVyx/Mzh+SgXQtc4FcHKjD1zcQ3dyhNGJHmv9VS/XEtcD7n"
     "xsSK4/AoC/R2mKGLh1WyOhP8tz4nN44i46hPKu9guz23OcMOAUeZXRJP20P6V6Ht"
     "+Z2+EUcmWxOYq10WirA3HFedRf+Ilja/u8oHe2+HY9fQMmrWXWwM8bNuOeJrMS45"
     "ACcU3jjA7BlmhhLonXIWE2RSx6k3HP2CMO2EGB30rlz/cBMA67H1M3pL1lX4BY1L"
     "abD1syg2XBTEUXNNawvxNAfbFznX3Ch7a/Wf8y7BTxcqEPPMyujr/WurLpqfLYJu"
     "BNRSAZMtPYb8fvzf70IdpmvvuSDG972gp5X9p+aJJNjMjDRs4iMv2RIaIblVkW8L"
     "kXkZDK1AiAtw4nrSDthoSLuCEzkQWOnYKgfGEttY29I7VRBHBRVnYn4YY6ZGPwkO"
     "VDJevw1ieifvgOjb2UsGWjdaJdAIEnfUbwlQlz3IHcPfjEUwVsbTZKtm88BelpzD"
     "xO/DfGuLOnl/s0nPPeKJn6AwS4W5nJQkeY99a6lFaA8r0PHaHMtpuMpJYm3I0GNi"
     "3WAPWKqPobwFpWaizxt2soRksUw5UsAwuvCMSwKwtrc="},
    /* CA Disig Root R2, sha256 e23d4a036d7b70e9 */
    {{0x50, 0x9e, 0x50, 0x3e, 0xbc, 0x5d, 0x28, 0x4a},
     {0x54, 0x00, 0x00, 0x21, 0x19, 0xb3, 0x75, 0xf9},
     3, 65537, 512,
     "oqPEAAnWhV0tbRT2wsNznjXCcVV+gfurRlDgwXxJeOareVg82v98HJ/YlwJ4PmtB"
     "BOlBvb4DLEX2L2TUq12jRz1km+lomsbMGz+6vrKLNAIumFUZ/Ixvql/aTM5NAyGj"
     "2NI0k1aWy0wMABY8XxrNyMdspq3TMae86OXhZtbS+wO0QWXJEK4OBWPGgGppMP3S"
     "7pDvDSffn5Vz9OEl2mwW3kE4NOqL/NHoBBRhLUF+rMd3TstRVPtekhgbBFpoxsnE"
     "+rcToJi3ESu31lfMfJ4X0csl/oZOJC5WDHhNngESpiunAWVufGIdhITf6sBrtaUq"
     "lYPDUxEMcx0LskaQ0UI6zkBula3/xpStbpeEjn1vnoqADUltc+J7kh7D88Hz6y4F"
     "b9kbzzd2BMi0WuQXp8vddh/QGXboLAWz1pw02JbcYYeRBeRECDPB2rkIZdSusjYN"
     "67o4ugzlm57rjWbdmc/WiUH2BJKKKSltazoc53V9AnEO88DnvcsZ3Z1gssJmYLax"
     "BO7J5oa5mmZAqOcR7YFFA4v2Z1nowQYRvd3PgAJPZUB4XEdQyJvmH4F75ESoW4Wa"
     "4t5a1cf5OkRmS+QyVHzkbJyzDj0XorI0EtZ+sqhJu9F6KEC+ohYf3+Q3HxFz+5AK"
     "ZUOiDXz4BgFVM32wDbj09a6lQld8NhGMe17EA52MeZ0="},
    /* Certainly Root E1, sha256 b4585f22e4ac756a */
    {{0x25, 0xd5, 0x6a, 0x20, 0x50, 0xf6, 0x0d, 0x8a},
     {0xce, 0x1f, 0x3d, 0x9f, 0x0d, 0x10, 0x94, 0x7c},
     2, 0, 96,
     "3m/4fxzf7flHh4axpMCK+IKXgOqPyEpeKn2IaKcBYhSRJHpcnqMXfYqGITQYUBsQ"
     "3tA3SybHGWCA6TS9YBk2QNYphwk8kXr2vBMj3VlOBF7PyAIcGFPBMdjaIOlEjeR2"},
    /* Certainly Root R1, sha256 77b82cd8644c4305 */
    {{0xde, 0x5e, 0x4f, 0x27, 0x4f, 0x77, 0x76, 0x62},
     {0xa9, 0x6f, 0xfd, 0xbf, 0x83, 0xa2, 0xf7, 0x92},
     3, 65537, 512,
     "0DbUH+rdq+TRtub7IsDdEw1qeyITHJc8aGNmMpwDtY2kgYPaeDARz9yyK76Sv47k"
     "xBO+pGhM2gJoFnS+st0E5Gsq3TcfYCzb9fehfJW3DHCGLvE671L3zNOb+Yu+Dt8x"
     "t51oXJKm9eXzCjS1/3ui5Iehxq8XAO8Dke2pHE5xPdKLbIn0eIbmakmgzrXSsKub"
     "9vTULuNy+TbG6xW3JYw6/CUNsyJzIXTISpZhkvUvCxil9K3i7kG9AXn6loyNFwIw"
     "tPmveBqMtDYQEAcFcND0MZCKUcWGJnmyEYhexfAKVM1Jpr8CnNJEp+3jeO9GXm1x"
     "0XlwHEZfUenJN9xffml7Qd80ReA7hPShigo2njfMYlLhiQ0o+XojsQ09PZr9nYHv"
     "LJDAe0ROu0ngDkpWkrzLtd15F4mR3mGJdJKo4zKFvk6FpEtZyyvFeI5xVNACN5mM"
     "5Unq4FRypBEGLwuMwVu+taGwU26cuGCRH1lr+S30lAqXtezFdgNUG2VSukySVlE1"
     "oEDYKduuUnY7LTBAm4rQQla0t4gBpIc7U5bNoxaP82aqF7HHYODBQwUM7ptbYG8G"
     "XIdbJ/lAEZ6cM8G35TVXBX8nzhcgjBz88fvaMSlJ7fULhKdPwfZOwiic+u7grwf7"
     "MxF6IU8LIRC2QDqrIjoEnIubhIZymtKnpcS0dZGpKyM="},
    /* Certigna Root CA, sha256 d48d3d23eedb50a4 */
    {{0x21, 0x1e, 0x93, 0x83, 0xe0, 0xe2, 0x63, 0x62},
     {0xb0, 0xc0, 0x68, 0xf1, 0x78, 0x90, 0x6c, 0xc6},
     3, 65537, 512,
     "zRg5ZRpZsepkFg6MlCSVfIPTxTkm3AzvFleN19iso0J/gsrtzVvbDrct7UUIF7LZ"
     "s8vWF1JyKNuOTp6Ktgv5noSaTXbeIilc0rPSBj4wOal0o5JWHKFvTAogbZ8jerTG"
     "2izkHSzcsyjQE/JMTgJJoVRAnublBaAthMj/mGzQ64oahAget2gj7iPVcM5tUWkQ"
     "7qF6wtEiMcKChdLyVXZQfCV6yYRcC6zdQk4r54KiJInLkLLQ7iO6Zky7YqT5U1pk"
     "e3yY+qNIng+VrqcY9GrsLgNFr/B0+CrNel3RvkQmMinx8fVszH4CIQufb6Q/vp1T"
     "4s99qSx8WBqX4T03NxhmKNJAxVGKjMMtzlOIJFhkMBbFquDWCqZA33j29QR8aROE"
     "vNHRpwbPAfdowKhXuzphrQSMk+Ot/PDbRG1Z3ElZrqyamTYwQXt2MyKHo8KShm75"
     "cO6uh4eVG8R6vTHz1NLlmf++SOx19XgWHaZwwX88G6GS+8/IPNbFkwqP9VU6dpXO"
     "WZiKCZV3MpqDuiwEOpe91C++12ybosp9bSbJVdXPw3lSCAmZByQtZCVrpiFpm2rd"
     "dE1rl3pBvasX+ZAXSI82+S3VxdvuqoVFQfrNOkWxaOY2TJuQV+wjuYcIwsQJ8ZeG"
     "KihN4nTA2sSM29/ioRdZziRZdDHaf/0wbdnc4Wrh/F8="},
    /* certSIGN ROOT CA G2, sha256 657cfe2fa73faa38 */
    {{0x80, 0x1b, 0x0f, 0xe2, 0xf5, 0x40, 0xf2, 0x59},
     {0x44, 0xc2, 0x23, 0x2b, 0x02, 0x33, 0x44, 0x70},
     3, 65537, 512,
     "wMV1GZF9RHR0h/4OO5bc2AEWzO5jkecLb847CmkafMLjr4KOhtdej1fr0yFZ/Tk3"
     "QjC+UOq2D6mI2C4taSHn0TcYTn2R1RZfa1sAwjlDDTaFUrlTZQ8dQuWPzwXT7twM"
     "Gtm4i3giZ+RpsGjFPORsWkbnzcf678TsS71qpKz9zChR75K0KaurNZpM5MQIxibM"
     "+Gmf5JzwKdNc+cYWJZ4jwyDBPQ8/OECw/oJEOKpaGoprY1g4tBXTthFpex5U7owa"
     "Iqxylz8jWZvJIoTBB0/Mf+JXyhJwu6Zl82l1Y72V+xuXzeSor/bRTqjZinEkzTY9"
     "vJbE8WypruXPDW4oDbAOtcpRe3gUwyAvf/sUVeERmf3VCqGeAuNiX+s1Syy4cug+"
     "PU+sLLsuhuKjdo/lkyrPpavIXI1LBv8SRqx4yxQHNeCp34vprxVPFolbvfaNxlmu"
     "iIUOwYnrH2fFRY7/bTc2K3hmg5FRKz3/UXd2YqHsZz4+gYPgVqlQHx96matjv4QX"
     "d/ENO9/3nGGzNZiKOrLsPBo3P36Pks/ZEhRk2hACFUH/T8TrHKPJ+pn3RunhGNmx"
     "uDItyxQMUNiDZYPuuVzPywVaTPoZl2vWXRPTwlxUvDJzoHj18W0ey5+lpp8i3NFR"
     "noJ5ZGApEz6j/U9yaqvi1OW4JFUsREuKiEScyoTTKjs="},
    /* Certum EC-384 CA, sha256 6b328085625318aa */
    {{0x88, 0x46, 0xc8, 0x13, 0x43, 0x34, 0xdb, 0xd7},
     {0x49, 0x86, 0xf4, 0x33, 0x05, 0xca, 0x93, 0x57},
     2, 0, 96,
     "xCiOqxhbar5uZDdj5M3sqzr3zKG4DoJJ14Ypn6GU8uNgeJiBeAZN8uyaDldgg5+0"
     "5hcvGrNdAluJIzzCEQUqp4gTGPNQhNe9NCwniVX/zkzn36YfKMTwVMO5fLdTrevC"},
    /* Certum Trusted Network CA, sha256 5c58468d55f58e49 */
    {{0xf6, 0x8b, 0x48, 0x0e, 0x36, 0x60, 0x44, 0x05},
     {0x40, 0x65, 0xcc, 0x01, 0xc1, 0xbe, 0x30, 0xcf},
     3, 65537, 256,
     "4/t9o3K6wvDJFIf1awFO4W5AB7ptJ11/91sts1rHUV+rpDKmYYe2bg+G0jACl/jX"
     "aVehGDldamR5xgFZrDwxSjh80gTSSyjoIF87B6LMTXPb865Px1bVWqeWifrzq2jU"
     "I4ZZJ88JJ7ysbnKDHDBy3+Ci6dLhdHUZvSqeexVUBBvXQzmtVSjF4hq79MDkrjhJ"
     "M8x2hZ85RdKknvISjFH4fOQtf/WsX+sWn7Et0brMkUJ3TCXJkDhv2/DM+44el1k+"
     "1WBO5gUo7Ul5E0u6SNsv+XLTOcr+H9g0cvW0QM8xAcPs3hEtF10fuFDRXhmnad4H"
     "MyjKUJX5p1TLVIZQRan5SQ=="},
    /* Certum Trusted Network CA 2, sha256 b676f2eddae8775c */
    {{0xc8, 0xf5, 0x2e, 0x90, 0x60, 0x8a, 0x9b, 0x66},
     {0x49, 0xe9, 0x62, 0x56, 0x0c, 0x36, 0x9e, 0x5e},
     3, 65537, 512,
     "vfl4+ObVgAxknYYblmRnPyI6HnUBfe/7XGeMycxca6mR5rlC5SBLm9qbe7mZXdmb"
     "gEvXhEArJ9PoujC7Pgkap0mV7ytAJMKXx6fumyXvqAoAl4Vaqp3cKcniNQfrcE1K"
     "1sGzVrihQTib0fsxf4/gX+GxPw+OFklg1waNGPmqJhCrKtPQ0WeNG0a+RzDVLnLR"
     "xWPa52N5RH5LYySJhi40PylMUosqp8DikSiJucBb+R3Z5yet/5oCl8HGUJKbAiy9"
     "qbk0WQq/hEr/3/6zn+vZnuCYI+yma3cWKtvMrTscpIfcRnNeGWJoRVfkkIJCu0LW"
     "8GHgwaM9ZqNd9BjuiMmNF0UpmTJ1AjHuKSbIawLmtWJFfzcVWiNoidQ+3k4nsPBA"
     "DLxNF8tNorMe0AZa3faTz1d1mfX6hhpneLO/lv403L3nUlbls+V1e9dBkQXcXWnj"
     "lQ1DufyDljmVe2yAWk8TcsbXfSl6RLpSpCrVQUYJIP4ioLZbMI28iQzV13D4h1L9"
     "2u+sUS4Hs07+0AnacO+Y+lbmbdu1V0vc5SwlFcieLnhO+NqcnoYsylfzGuXIkosa"
     "gpZ6w7xQEmnYDlpGizrrJvojybawgb5CAKT41v4wLsfSRvbljnX98sy50IdbzAYQ"
     "YLuDNbdeZ95H7JlI8aShFf6tjGKOOVVPORa5sWOd/7c="},
    /* Certum Trusted Root CA, sha256 fe7696573855773e */
    {{0x06, 0xd9, 0x9e, 0x22, 0x6e, 0x1f, 0xf7, 0xdc},
     {0x4e, 0xaa, 0x3e, 0x02, 0x84, 0x9f, 0x85, 0xcf},
     3, 65537, 512,
     "0S2Ou7c26m03kZ9Ok6cF5CkDJc4cgvd8mZ9BBs3to7rA2wkswXzfKX5LZS+Tp9QB"
     "awMoGKPYnQXBKthF8ZHe3zvQgAKMzzgP6qdceBGkwciFXCXT07LnJc8RVJerNcAe"
     "dhzvAFOfOdwUpSwiJbNycvyNs+U+CB4UKjcLiDzKsPTIwqGuvMG+KWdV4vytWVz+"
     "vVcssJCNwu03tnyZiLXVA5o9FQ09OqioRfCVTiVZHc2YabvTzDLJje+B/q19ibu6"
     "YBPKZZVnoPMZ9gNW1GrTJ+KhrYPwShIidxwFc+IZcULA7HVGmpBY4GqOK6VGMASO"
     "GbIX476pun9W8SQD17IhKHYONjBMedVBmpqouDW6DDryRBsgiPfFJdc9xuM+Q92H"
     "/sTq9VM+TGX/O0rLeFprF18Nx8NPTpoqou1XTSLiRpo/D5E0JH1V44yVN9Ma8Akr"
     "LNLJjbQNAKtnKSjYAfUZBLYdvnb+clzEhcrSgEHfBaij1YSQTwvz4D+bGdI3iT/y"
     "e1IcjPbh9zwHl4wOolmBDLKQPdPjWUbtD6mn3oBrWqoHthnLvFfzlyF6DLErdD7r"
     "2qdnLUzEmJ42CXZmZvwaP+pIVBy+ML2AUL98tc4A9gxh2eckA+DjAYEOvdiFNIi9"
     "sjaoe1wI5USAjG/4L9Uhyh0c0PvEtYfROk7HdrU1SLU="},
    /* CFCA EV ROOT, sha256 5cc3d78e4e1d5e45 */
    {{0xb2, 0xc5, 0x6b, 0x25, 0x5e, 0xb9, 0xb7, 0x9d},
     {0xc5, 0x13, 0xbe, 0x07, 0x6f, 0x32, 0x07, 0x92},
     3, 65537, 512,
     "111rzRA/HwVZ1QVNN7EO7JgrjhUd+pNLF4IhcRBS11FkcBbCVWlNjhVtn78MG8Lg"
     "o2fWDKzPIq6vd1QqS0yKU1J6w+4u3rNxJcHpXT3uoS+j9yo8ySMdaqsdoafx8+yg"
     "1UTPFc9yLx1jl+iZ+f2TpFSATFLUUqsuSd+Qzbhfvj/eocpNINQl6IQpU7exiB//"
     "+tqQnwqpLUE/sfEYKe4WWSw0SRqoBteoiNIDcnoy4upoTW4slmV7yln68uLd7jAs"
     "+8xGrMRj629/Nis0cxKUf9/MJp7xcl1QZVmPabOHXjJvwxiKtZWPsHo33lpFO8c2"
     "4e9n0TnTl1tzYhlILYccBvt0mCBJc/AF0huxoKO3G3DTiGm5WtY49GLcJYt4v/jo"
     "frhcyZVPX6ctuSBrz2vd9Q30grf0smYuECj2l1p7lhaPARktbG5/OVgGZIMBg4PD"
     "TZLdMsaHpDfpFs6qLWivCoFlOnDBm61NbVTKKi1LhRuzgOZwRQ1rXjXwfzu4nOQE"
     "cIkSJZPaCpkiYGpjYE52BphOvYOtHViKJYXSx2UeLY7G37bG4X+KBCEVKXTwPpyQ"
     "nQwu8Yo+WqoMCR7H1Tyj7ZfDHjT6OPkIDuPAXSuD0VZqybaoVFMueDJnPYJ/dND7"
     "4bYFYLlw244L+RNYb3FgEFIQucFBCe9yH2cxeP+WBY0="},
    /* COMODO ECC Certification Authority, sha256 1793927a06145497 */
    {{0x1a, 0xcb, 0xd5, 0x96, 0x14, 0x61, 0x1d, 0x5c},
     {0xcf, 0x1d, 0x31, 0x5f, 0xc3, 0xa7, 0x55, 0x88},
     2, 0, 96,
     "A0d7L3XJghWF+3XkkRbUq2KZ9T5SCwbOQQB/l+EKJDwdAQTuPdKNCZcM4HXk+vt3"
     "iir1A2BLNosWIxatCXH0SvQoULT+iBxuP2wvLwlZW6VbCzOZ4sM9iflqLO+y0wbp"},
    /* COMODO RSA Certification Authority, sha256 52f0e1c4e58ec629 */
    {{0x7d, 0x5e, 0xe0, 0x36, 0x75, 0x55, 0xe5, 0xcd},
     {0xe2, 0x08, 0xff, 0xb2, 0x79, 0x88, 0x45, 0xe9},
     3, 65537, 512,
     "kehUktIKVrGsDSTdxc9EZ3SZKzejfSNwAHG8U9/E+ioSj0t/EFa9n3Byt2F/yUsP"
     "F6c947AEYe7/EZfH9IY+Cvo+XPmT5jR62RRr55yzhaCCenavcZDX7P0N+pxs+t+w"
     "gvQUfvm+xKYvT3+Zf7X8Z0NyvQwA1onrayzT7Y+YHBSrfuXjbvzYqOSSJNpDa2K4"
     "Vf3qwbxstovzDo2a5JtsaZn4eEgwRdWt4Q08RWD8MpZRJ7xnw8outmvqRsfHIKCx"
     "H2XeSAi6pE6p8oNGN4Tr6MyBSENnTnIqm1y9TBsoilwie7SrmNnu4FGDwwlGTm0+"
     "mfqVF9p8M1dBPI1R7Qu2XK8sYxrfV8g/vOldxJuvRZnio1oktLqpVj3Pb6r/SVi+"
     "8Kj/9Lit6Tf7urj0Czr56ENCHonYhMsT8dm74YlguIwoVqwUHZwK53Hrzw7dPamW"
     "oUi9PPevtQ0iTMARgexWO/bTouJbt7IEIlKVgJNp6I5MZfGRAy1wdALqi2cVKWlS"
     "ArvX31BqVUa/oKMoYX9w0MOiqiwhqkfOKJwGRXa/ghgntNWutMtQ5mv0TIZxMOmm"
     "3xaG4Nj/QN370EKIf6MzOi5cHkERgWPOGHFrK+ymircxXDpqR+DDeVnWIBqv8mqY"
     "qnK8V0rSS527EPywTEHl7R09XiidnMy/s1Hap0flhFM="},
    /* D-TRUST BR Root CA 1 2020, sha256 e59aaa816009c22b */
    {{0x43, 0x95, 0xa1, 0xe9, 0xaf, 0x93, 0x45, 0x92},
     {0x3e, 0x9a, 0x63, 0x79, 0x43, 0xb9, 0x7c, 0x88},
     2, 0, 96,
     "xsvHKNH7hPWa70IUIOFDa251rfwrA4TUdpMl11k7QWVrHuY0Krt09hLO6G3nq+Q8"
     "Tj9ECIvNFnHLv5KZ9KTXPFBUUpCFg3iUZ2ejHAkZPXU0hd7tYH3HDLRBUrlu5e5C"},
    /* D-TRUST BR Root CA 2 2023, sha256 0552e6f83fdf65e8 */
    {{0x8b, 0x62, 0x77, 0x08, 0x66, 0x9b, 0xc8, 0x89},
     {0x86, 0x3c, 0x8b, 0xf7, 0x7f, 0xe4, 0xa4, 0x18},
     3, 65537, 512,
     "rv8JWZGACkpo5iQ/uKfkyDoKOhbNySNhoJNx8quLc4+gZ2Vg0lRrY1FvSTPgcgcT"
     "fTjNBpIHKVJrTndsBNOV+t1MjNldwWF9S+cos0SBe1Gv3TOxaHzWTkz+K2i5ymZp"
     "xOxeV3/3DcecNjblB2CswEzqCGzvBnxPWyh6CPyTXZv2nLSLhrohufTw6FlaKKE0"
     "hBolkba1j++y+YD6+T08EXLY4y+GdsV5LMGpkJNGmGfLg2qgUCOnO/aBOeDt8Lm/"
     "ZfHYy3r773MDzgD0fdfgXTtmuNyOuoPLh3YD/CXZ5yNvBv1n8+D/hLxHv7UWGEZp"
     "FMwF99vTSaxrzKvktQtDJF5La01n39a1Pk94H5RxJOrecPzxk/6ek1rklFqXVAw1"
     "e19s7gAfJOwDugL1dvSf1JrthSw4Ii/H2C92EU/9bFzo9Y4nh38ZSiFHkB15jRxb"
     "+M9KheTts1uNvsRkKF1BxG6sOFpPI3R0qRLD9tK5ERUzB5HYOzc6YzAG0cUiNihi"
     "IxDgRsyXrNYrXWQk1e4cDt77CFp1KvZjbc4LQr7RunAcnCHlDzFpF9f8CrTe7YCc"
     "y5K0i/XeWaJYCaVjRwvhQTI0QdmasdmosBta3g0N9OKyXTWAuYHUhGmRAst10I3F"
     "tT0JkQmPFKEUdHk+1skVHaRZWSLc9opFPTwS1j5dMi8="},
    /* D-TRUST EV Root CA 1 2020, sha256 08170d1aa3645390 */
    {{0x8a, 0x9d, 0x37, 0x5d, 0x77, 0x2f, 0x17, 0x18},
     {0xbf, 0x2e, 0xb1, 0x8c, 0x42, 0xaf, 0x92, 0x05},
     2, 0, 96,
     "8QvdhkMgGd+XhegiSpvPnZi/tAUmycvjptKPxZ54ezGJqYmtJzxlEIL838OdTvAz"
     "I8TSMvUcsN8zF13F8LGK+e+5txTKKUrCD6l/dWVJKjBn9GT31hp32sPCl2FCe0mt"},
    /* D-TRUST EV Root CA 2 2023, sha256 8e8221b2e7d40078 */
    {{0x87, 0x28, 0x45, 0xd0, 0xe0, 0xc0, 0x45, 0xbb},
     {0xcc, 0x6a, 0x8c, 0x23, 0x0f, 0x91, 0xbb, 0x9e},
     3, 65537, 512,
     "2I6jiYALsldS3KlTTDe5f2MXE++nWyNbaXWwmQoXwYvE26jgzDG6wvLNXem3+B2v"
     "asSVh9dHyZXYggRQPYEI/+Q9s7HWxbL9iAnbnITsJRcUh38weJtqWMm2cyg8NPeZ"
     "93/TpvgcRXytLIyUP9hnEFN+Is1OJVHwJSQ1EV4QxuyHZomBaLrMK51Hcx+9zZGk"
     "cmqcohsYoG/sUPR9QMKoMM+9c8gTKxATHouaqDqUc9MYaQpK/8EBA/95f7VIf3vu"
     "6ClvNkyVYYbY+aJziu6uL5buaM09TShC+UUrMhtGVRZqpksp+buVVr9GHewdkx3A"
     "ZbIfoUOuVp6gsY9rErdgbXgLyopc7R6WDoOmSJWNO6MhxK5YxgCyhLQjpJaGNbjY"
     "ntisNEmYY5XFy21IR+LyLhge0DGr3XTs+dyMuByOaCO60PNQ3M9lj3M6Msd8/sqC"
     "Ik++jmJHZuXNh+Lo1Q8Yn+UEcktGPBDyRMJkVnFOdeicySZ0xX1Z0QpbD23+nnUc"
     "GMYaOnzYDQTMzbdFZXqxj7iuhEg+s3pNqAPi4n4BFlloGEMzsNLcsBpDNe6l2qlG"
     "XK6GgUEBSnQm7J8Gv8IFN2R1eClo/cX16/5H+eSFsOF7MZ2mf3KjucQsLsyZVw4h"
     "DEUBlGXrZQnGYyILM0mSSDz8zc6wPo6ei/j+ScU1ckc="},
    /* D-TRUST Root Class 3 CA 2 2009, sha256 49e7a442acf0ea62 */
    {{0xe3, 0xe2, 0xca, 0xec, 0x65, 0xaa, 0x56, 0x0c},
     {0x9c, 0x45, 0x46, 0x46, 0x98, 0x19, 0xd3, 0x1c},
     3, 65537, 256,
     "07JKz3pH73WbI/o6L9ZQRYk1OsZr2/7bAGio4AMRHTdQCJ9NSmiUNbNT0ZRjpyBW"
     "r95ReOwqPfNISFA+Ct9GVYsnbcMQTQ2RUkPYh+BdTja1IcpfOUAEX1t+zKPGK6lA"
     "Htk2hNZI85IeNEYgJMGkUY5KGu9QP2ldGX9Fw8cBj1HJI+hyrrS8Vgl/Esscsa8p"
     "kArJVcwP07Qa7Uc1WkrtnHMEIdCqvQwTtQDKJmzEawyUWpWU2lCa8f+lK2YxpMk4"
     "oN8dH7gJLvOn6GdSq5Uf4EY+2KTDylrFMYDoSJqflGn+Gd3Yc3yBypbeju2zMgVl"
     "hDTm5v1XELVfdr8vsBANxQ=="},
    /* D-TRUST Root Class 3 CA 2 EV 2009, sha256 eec5496b988ce986 */
    {{0xc9, 0x01, 0x07, 0x59, 0x49, 0xeb, 0xc3, 0xfe},
     {0x53, 0xea, 0xce, 0x6e, 0xf1, 0x91, 0xa6, 0xcc},
     3, 65537, 256,
     "mfGENHC6L7cwoI69fATPvmK8mf2Cl9J6CmeWOAn2EE6VInOZjdoVLecF/BlzIreO"
     "mAC8PD2soWz71nklS63wzGTaiD4puA8J0zTdM/Vi0eHNGenuGE9MWK7iHtYMWxVa"
     "2Dq4xBhkHuMzsrWJd04Mv9mUaxOXbxKj/pmpBMwV7GBoNu0Ie7f1v5PtZjGDjMZx"
     "NIdOF+qvi5GNHFZBriI3XjfyHdnRLQ0vaVGnvmamijoqvccaseEU8L46HbnPW7Fq"
     "/rSxRiCi+x47cO+TmH2Mc5byxe+FcK0pJvweBD4coNgPy1KDYnzui1OVkKlXouph"
     "Bdj5TcQn+m6t7fnXUfdrpQ=="},
    /* DigiCert Assured ID Root G2, sha256 7d05ebb682339f8c */
    {{0xdd, 0x8e, 0xb0, 0xf4, 0x76, 0x56, 0xc6, 0xfa},
     {0xd8, 0x40, 0x43, 0x78, 0x6d, 0x04, 0xb3, 0x9f},
     3, 65537, 256,
     "2ecoL1I/NnJJiJM08/hqHjFUgJ+tVEG1R9+WqNSvgC25Cs91/YmlfST64yIMK7yV"
     "FwszvxlNQQaQAL0MTRD+B7XnHG4iVTFll73TF9IeYvPb6mxQjD+EDJbPt8sD4Mpt"
     "oRRMG4nd7QCwUnyvkWyxOBPR6RIIwACwHCsR2ndwNpuuznmH3IJw5gl0cFVpr6No"
     "n7/dtnmz8p1wKVX0q/+VYfPJQG8d0b6Tu9OIKrudv3JaVnE7P9Tz0Qr+KO+j7tmZ"
     "rwPTj2C38pKhsb2JiR8wzcOmLmIzrhYCd0Ra54EKPKdELnm4PwS8XKCH4RuvUY7N"
     "7Cz6+P5t8Dp8qovkZ5UxjQ=="},
    /* DigiCert Assured ID Root G3, sha256 7e37cb8b4c47090c */
    {{0xa6, 0xd1, 0x4b, 0x8d, 0xbd, 0x31, 0xb4, 0xd6},
     {0x0c, 0x67, 0x3e, 0xf6, 0xd4, 0xdb, 0x1f, 0x39},
     2, 0, 96,
     "Gee8rERl7c24P1j7jbFXqUQtBRXy7wv/EHSftWJSX2Z+H+XcG0V5C8zGUwqdjV0C"
     "2alZ3gJa9pUqDo04SopJxrzGAzgHX1Xafglu4n9e0EUgD1l2ENagJPAt3jbybCk5"},
    /* DigiCert Global Root G2, sha256 cb3ccbb76031e5e0 */
    {{0xce, 0x6a, 0x17, 0x2b, 0x6d, 0x7d, 0xfe, 0x6d},
     {0xf9, 0xa9, 0x4d, 0x3e, 0x6d, 0xaa, 0x36, 0xf0},
     3, 65537, 256,
     "uzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI2/Ou8jqJkTx65qsGGmvPrC3oXgkk"
     "RLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx1x7e/dfgy5SDN67sH0NO3Xss0r0u"
     "pS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQq2EGnI/yuum06ZIya7XzV+hdG82M"
     "HauVBJVJ8zUtluNJbd134/tJS7SsVQepj5WztCO7TG1F8PapspUwtP1MVYwnSlcU"
     "fIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQvIOlCsRnKPZzFBQ9RnbDhxSJITRN"
     "rw9FDKZJobq7nMWxM4MphQ=="},
    /* DigiCert Global Root G3, sha256 31ad6648f8104138 */
    {{0x54, 0x43, 0x1f, 0x47, 0x9f, 0xe2, 0xa4, 0xe5},
     {0x3b, 0x43, 0x8e, 0xca, 0x16, 0x69, 0x61, 0x29},
     2, 0, 96,
     "3afZu4q4C/sLfyHS8L6+c/MzXRq8NOrexpu80JX28MzQC7phW1FGfp4tn+6OYwwX"
     "7Adw9c+ELkCDnOg/QW07rdOkFFk2eJ0DQ+4QE2xy3q6Ip6FrtUPOZ9wj/wMco+I+"},
    /* DigiCert TLS ECC P384 Root G5, sha256 018e13f0772532cf */
    {{0xae, 0x63, 0x77, 0xbc, 0xda, 0xf8, 0x7f, 0xe1},
     {0xa9, 0xe7, 0x69, 0x59, 0xff, 0x71, 0x9a, 0x17},
     2, 0, 96,
     "wUShzxGXUJreI4I1B83Qyxid0vF/dzVPO92UclLtwjv47Pp7a1gg7Jmuyfxos3W5"
     "2wnsyBP1TsYKHWYwTLsfRwo8YRBCKXylCA7gIunTNWjOm2OfhLWZTVigjvVU55XJ"},
    /* DigiCert TLS RSA4096 Root G5, sha256 371a00dc0533b372 */
    {{0xa6, 0x30, 0xcf, 0xc3, 0xa7, 0xbe, 0x67, 0xd2},
     {0x39, 0x12, 0x03, 0x97, 0x6f, 0xb0, 0xe1, 0x9f},
     3, 65537, 512,
     "s9D0yXkRnf38ZoHnzNXkvOyBPmo1ji63596v+QdNzzCd6gkLmb1sV9oYSrh4rDo5"
     "qKZIrC5y5b3r8RrN56QDqT8RtNgviRb7lAE9uy/4EwWheByOKOBF4IP0WRuVs65+"
     "A0XlvsJC/u7yPLaFE5gynRaoKcILHDjcnzF3XL8no/wnrLcrvXSbFy3ygdpdsOEj"
     "Fz6IShIj0OrPnd4DF7FCSqAWTKRtk+k/Ou46fJ1YnfROj/w7I8htuOIF2szr7MMx"
     "9NenKVSAz0RbTG8wnvPM3R+UQ51Nf3BwDdQ60TfwbJ2bwBSTWO/NQTh1vBMDlXx/"
     "41zp1Q3V4nwQYqpr8D128z+j6LDB/e+qV02shqcYtCnBLA6/ZL4pjNgCLc1cL/J/"
     "7xX0DBWsCrDx0w1Pak13lwGg8Wa3t87vzuzspXXKrOPhY/e4oQTIvHs/XS0WIlbt"
     "SEn+py95MCWbumstP507xBfnHS778s+m/OMULJaYIYy0kekZYIPyMCsGc1DVmDsG"
     "6ceKDGCMKPhSm27h9k27BiSb1ysmP/0qL3H11iS+fzGeD23oj09Noz//NerfSV5B"
     "j4b58Xd5Sxu0o14v+0YC0GYTXl6FT87YcIh7zgG1lpfXzX39gvjCJMHKATlPjaLB"
     "FEAfnGbVDAlG1vLQ0Uh2VjpDy7YKETm6jBNsBrWez+s="},
    /* DigiCert Trusted Root G4, sha256 552f7bdcf1a7af9e */
    {{0x88, 0xa9, 0xc0, 0x37, 0xef, 0x37, 0xd0, 0x41},
     {0x18, 0x96, 0x99, 0x9b, 0x60, 0x11, 0x59, 0x09},
     3, 65537, 512,
     "v+aQc2jeu+RdSjwwIjBpM+zCpyUuySE98orYWcLhKac9WKt2ms2uexuEDcQwH/Mb"
     "pDgW61bGl20dq7J58soR0uRf1gU8Ug9SH8aeFaV+vp+pVxZZVXKvaJNwwrK6dZlq"
     "czKU0RBEEC7fgvMHhOZ0O21x4i0MG+4g1ckgHWMpLc7sXk7Ik/ghYZs06wXGXuxb"
     "Grzryc/NrDRAX7F6Zu53yEioZldXn1RYjgwrt0+nMNlW7sp7XeOtyU9e5TXnMcva"
     "k17cjo+A2raRmECQecN4x7axxLVqGDgDEI3Y1DekLgV9iPWCPhCRcKtVgkEy19sE"
     "cypukQF8IUzUvK4bA3VdeGbZOjFEmjNAvwjXWkmkwuapoGfdpCe8oU85tRFYF/ck"
     "XEaPZPfBaYh2mHY9WV1CdoeJl2l6SPDgohIbZpp0yt5LHucOY67m1O+SkjqePdwA"
     "5EUlibaaRBkrfsCUtNJhbesz2cXfSwQAzH0clcOP9yGyshG3u3/y1YxwLEFgqrFj"
     "GESVGnZifvaAsPvoZKYz0YkH4b235kOkGLimdwHhD5QMIR2yVCkliWzlDlJRR3S+"
     "Jqy2QXXeeqxfjT/JvNNBERJb5RBQ6zHFynIWIgnffEx1P2PsIV/EIFFrb7GrhotP"
     "wtZFX50g/KEexcCPorF+CiaZ9eRpL5gdLfXZqbId5Rs="},
    /* e-Szigno Root CA 2017, sha256 beb00b30839b9bc3 */
    {{0x53, 0xc1, 0x6b, 0x02, 0x0f, 0x9d, 0xa0, 0xbf},
     {0xc2, 0x66, 0x31, 0x12, 0x51, 0x56, 0xc9, 0x9f},
     1, 0, 64,
     "ltw9itiwe2/GJ75EkLGzVhV7jkMkfRqEWe5jaLLGXofQFUgeqJCtvVOi2t46kKZg"
     "X2gytYZB34dbLHvF/nx62g=="},
    /* emSign ECC Root CA - C3, sha256 bc4d809b15189d78 */
    {{0x26, 0x3b, 0x69, 0xf9, 0xc1, 0x8e, 0x2d, 0x21},
     {0x04, 0x96, 0x07, 0xba, 0xa4, 0xe2, 0xd7, 0xe8},
     2, 0, 96,
     "/aVhrnsmEB3ptyIwrgb0gbOxQnGVObzTUuOvr/nylzWSNkYOh5WNuTla6bvf0P7I"
     "B0E8u1Vvg6Nq+2KwgYkCcH1IxUrj6SJUIk2Tu0IMr3ecI6Z912ERzmXH+H/+9fKp"},
    /* emSign ECC Root CA - G3, sha256 86a1ecba089c4a8d */
    {{0x94, 0x6f, 0xd2, 0x10, 0x51, 0xa4, 0x46, 0x99},
     {0x86, 0xd6, 0x38, 0x68, 0xe9, 0xe7, 0x3e, 0x17},
     2, 0, 96,
     "I6UMuC0S9SjzsbLd4gISgJ45X0lNn8klNFl07LsGHOfAcq/ori/hQVSHFKhKsuh8"
     "guZbarXcs3XOiwbQhiO/RtWODz8E9Ncckn72pWPC9V+OLk+hGBkCKzIKgmR9FpPR"},
    /* emSign Root CA - C1, sha256 125609aa301da0a2 */
    {{0x11, 0x05, 0xfa, 0x70, 0x6a, 0xe2, 0xa5, 0xc2},
     {0x68, 0x2b, 0xed, 0x7a, 0x06, 0xdf, 0x0b, 0x4a},
     3, 65537, 256,
     "z+upufGZBczYKCFK83M0UYRWEPWgTywS4/oTmifQz/l5GnRfHXk5/Fv4cI7gklL3"
     "5CX5VIPZHdPIWoU/Xse2B+4+wM6ar6xWQio5JXDWv7V7Nq2s9nPczdcdioOl+yuQ"
     "FTdrHCZH3DspVpNqs8FqOp099cGXOFgFixwR4+S0uF2FHYP+eF8LRWgYSKVGczQ7"
     "/g/IdrvHGPMF0Ybzhe3nudkyrVWIzqa2kbBPrH4VI5b2P/AgNBbeCsbEBEV5f6f9"
     "vtKppa+cxSMq9zwhbL2vj07FOrLzNBL834AaSaTUqZX3noleoomslMuoaJuvimUn"
     "zYnu3Yy1aylwQ6BpC+S5Dw=="},
    /* emSign Root CA - G1, sha256 40f6af0346a99aa1 */
    {{0x82, 0x83, 0xf3, 0xa4, 0xc0, 0x7c, 0x2b, 0x55},
     {0x77, 0xdb, 0xee, 0x8e, 0x41, 0x06, 0x5a, 0xc8},
     3, 65537, 256,
     "k0u76WaK7p1b1TST0Bsew+eeuGQzf2N4aLTNLnF115sgxk0pvLZoYIr3IZpWNVrz"
     "dr3YzZr/k1ZLpVkGoZM0Kd0WNHVO8oG0x5ZOrRkVUkr+PHB1cM2vK6sVmjM8qrOL"
     "qs1D/fXqcP/tzxE7lM5OMhbTI0Aqd7OvPAEsbO2ZLIvZTmmYsvePQbAyeGHWDV/D"
     "+qJAkh1cF+ZwPjXnorfCYuKrpDhMtTk1b+oDafo6VGiFbdbyL0NVHpENDtjVaqSW"
     "0RM8LHhQ6DqS0hdW5TUaQBw+jSztOd9C4INBdN+jzcKGYEho42kLVACL5HZpIQ15"
     "TjQIXhTCzLG3rdd8cIrHhQ=="},
    /* GDCA TrustAUTH R5 ROOT, sha256 bfff8fd04433487d */
    {{0x2c, 0x32, 0xf1, 0x17, 0xe7, 0x11, 0x79, 0x64},
     {0x55, 0xde, 0x8b, 0xb9, 0xc2, 0x21, 0x7c, 0x0c},
     3, 65537, 512,
     "2aMW8Mh0dHeb7zMNOwZ+Vfy1YI92hhJCfVZmPoiC7XJjDp6L3TQsAlFRwxn9WVSE"
     "yfFrs0yw6ehGXTjGoqcuEVe6ghWinI9tsJlKCvLriXBjTnnEt1u9ol2x8kECK62p"
     "OqPseQrsXzrj/e+APK00mxqriCZ7VqKChh/rNYmDf1+uKU49tm7srsHwJ5uu4/Ts"
     "765/94Y9cnrrpftZTqfrlYwiOXnhLQiPzLyRuEH3FMEjqcOtmkVEs7LXLM3GKeJQ"
     "EK5cy4KOFxg2fZfmiJqwTTQJ9Cy5WmYqsBebnh52nUpmMUHfP/vFBu8btn4aRjb3"
     "ZGM74zkYI+dndRTVdVeSN72+ahsmUPI2JgaQxXABZG12ZuGR224HwGGALrIuL4xw"
     "p9E7PLOR5G62xDtw8mySlwnNR30YwPO7ng/Wi64HtloPzgsMR6flPri9fcebNaBh"
     "lzpBdRfMK5Z3KpIhHtmVdiBnaM8Nvd/WHwlqmuLMc3GkL30SgLdTMEZeS1SZD2fJ"
     "pcjyIMGC7J0R38IC+xo70e0gmu9lZJIQDSri3nDxGGeCjGHeuLzRL5z7D9Ar7Rt2"
     "ueQ5Vfj4oR24qoAATILnsn8JuLwwoC8N9VKejveSswoAHQBUlwbgsQfZxw9cZX08"
     "bVlX5O2ljelAU58VS6Bx9hoh49pwBiFYFIeFd3mqgnk="},
    /* GlobalSign ECC Root CA - R4, sha256 b085d70b964f191a */
    {{0xab, 0x2a, 0xb7, 0x98, 0xa8, 0x7d, 0x53, 0x59},
     {0xa2, 0x6d, 0x57, 0x88, 0x14, 0xc3, 0x93, 0xac},
     1, 0, 64,
     "uMZ5049sJQ6fLjkZHAOkrprlOQcJFspjsbmG+IpXwVfOQvpzofdlQv8ewQCybnMO"
     "/8ch5RikqtlxP6jUuc6MHQ=="},
    /* GlobalSign ECC Root CA - R5, sha256 179fbc148a3dd00f */
    {{0x0b, 0xb1, 0x58, 0x53, 0x15, 0xca, 0x1b, 0x46},
     {0xd3, 0xc7, 0xb7, 0xcf, 0x97, 0x39, 0xf9, 0x6f},
     2, 0, 96,
     "R0UOlvt9Xb/pOdEh+J8LttV7HpI6SFkc8GIxLcB6KP4ap1yztsyX50XUWPrRd21D"
     "osCHZTQKH3rd6zwzocWdTaRvQZU4f8kehOvRnkmSh5SHDDqFSmafnVmTTZdhBoZK"},
    /* GlobalSign Root CA - R3, sha256 cbb522d7b7f127ad */
    {{0x6e, 0x1b, 0xb1, 0x5c, 0x53, 0xcb, 0x26, 0x32},
     {0x8b, 0x79, 0x0c, 0x9b, 0x54, 0x48, 0xbb, 0xf9},
     3, 65537, 256,
     "zCV2kHkGeCIW9cCDtoTKKJ79BXYRxa2IcvxGAkPHsoqdBF8kyy5L4WCCRuFSqwyB"
     "R3Bs3WTR6/Usow+CPQwrrpfXthSGEHm7OxOAd4wI4UnSamIvH176lmjfiSeVOJ8G"
     "1z7JyyZZDXPesMjpJg6DFcbvW4vSBGDKSaYo9mk79svIKJHlnYphVzesdBTcdOA6"
     "7nIvLpz70Lu/9T0A4QYz6IIrrlOmOhZzjN1BDiA6wLSnoemyT5AuMmDpV8u5BJJo"
     "aOU4JmB1sp93/5EU764gSfytQBVI0QIxYRleuJfvrXe3ZJp6v1/BE++bYvsNbOBU"
     "aRapA9pu6YOTcXbGaYWCFw=="},
    /* GlobalSign Root CA - R6, sha256 2cabeafe37d06ca2 */
    {{0x21, 0x83, 0x6f, 0x12, 0xce, 0x79, 0x69, 0xff},
     {0xe6, 0xb3, 0x30, 0xb1, 0x3c, 0xf0, 0xdc, 0x08},
     3, 65537, 512,
     "lQfoc8pm+ewUyns89w0I8bRFCyyCtEjG61s8roO4QZIzFKRvf+kqzMawiGvFtonR"
     "xrL/FM5RFCHsSt0bWsbWh+5NOhUG7WRmC5KAykTec5RO86eJf094YwjIElBtQmYv"
     "Tbl5KE1SGooagLcZgQ5+xIq8ZEwhHENo1z08isWyZtWQmrcxBsW+4m0yBqYe+bnr"
     "qqO4v76CY1DQ8BiJ3+QPefXqoh8q0nAue+e8k7ttU+JIfIwQBzj/ZrJ3YX7g6ow8"
     "qrSk9vOVShIHbf2MsonP0KBhd8hYdLDUIzr3XTrKotudCd5dRC2Q8YHNV5L6frxQ"
     "BGM032uTGL5rNrI55KwkNrfw77YcE1eTtt6y+OKFt3OiuDWqRfLgnTahb1SK8XJW"
     "bi6IxVFCRBWU7qPFOJabTk5aC0fzBjZJdzC8cTflpuwhCHX85mEWP3fV2ZGXhAps"
     "1AJNdMAU7f05+4PyXhShBLAL6f7uj+FuC7IIs2FmCWqxBjplllnA8DX9ydoojRoR"
     "h3CBCqiadR2eOoYFAJ7bgNYl+dwFnidZTHY5W+r5paHYgw/R/98wEfmFzzNI9cpt"
     "ZBQselhP00sIScWVZBpjDnk99bOMylitnEJFeW4OhxlcVLFltr+Mm9wT6Q1vuC7c"
     "Z27JixG1hBSKABlwg3mRl5HUGie/Nx4yB9gUYzwoTK8="},
    /* GlobalSign Root E46, sha256 cbb9c44d84b8043e */
    {{0xd3, 0x17, 0x64, 0xd5, 0x5b, 0xcd, 0x0e, 0xbd},
     {0x13, 0x06, 0x95, 0xdf, 0xb9, 0xd4, 0x35, 0xb8},
     2, 0, 96,
     "nA6xz7fonlJ3dTT6pUanrTIZMrQHqSfKlLsM0goQx9qJsJcMcBMJAY7Y6kfqvrKA"
     "K838KA3brLykhjftcAgAdeqTC3suUpwjaCMGQ+ySL1OE2/tHFAfoX5RnXcl6gTwg"},
    /* GlobalSign Root R46, sha256 4fa3126d8d3a11d1 */
    {{0x5e, 0xb5, 0x19, 0xfd, 0xc4, 0x2b, 0x79, 0xdf},
     {0x7e, 0x83, 0x31, 0x3e, 0x8e, 0xef, 0x84, 0xa9},
     3, 65537, 512,
     "rKx0MuizZeW67UMmHaaJDUW6KYiypB1j3dPBLAlXiTmhVelnNHcMbuRVHVIl0hNr"
     "XuEdqbd9iTJfDZ6fLHpjYEAfprC2eI+ZVJYIWK7kBrxiBQIWv6+oIwO2lA+8bmzC"
     "y9Wmuwzp9sEC+yHeZt0Xq3RC7/B0LyX06mtVW5Dbnd9ehwpA+60Za/v3ymCI3trB"
     "j9au1X/UPIPu1xZMg0UzayfQhtAcLWvzq33xhan1KNKt7/OESxyH/BOjOnKiWhEr"
     "1idxJ+2BLW1mgZKHtBtYesw/CvpGT014XPgrSOMEhMtd9rRqs2X8Qp5RJiMgyz0U"
     "+YHtZRYATxpkl2YIz4x74yvAnfkU8hvxVmoWvyyFhc14OJrrQmoCNBiDF06UVvi2"
     "grXzlt09875/IHc+exkjayzUcnNDV33g+NdpTxc2BPnAkGA3Rd7mDNh0ja6com10"
     "XUK+BvXZZG4CEKyJsEw7B01AfiTFipiCeY6kp4IgjSP6J3HJ38ZBdKBN9pEW3EaM"
     "XyljMVlxDNhvwrYyffvmXVOmfhX8u3V8Xez49hcc7MdrGcvze/ArB6XZbHlUdmyd"
     "HKZuDul5DKgjaqPfGzAxn7FUe/5qy2aq3GXQop5Kmgcha4GP28RZ+t4iwASc46pb"
     "NpPoPb16oZ0LdrELx539z5ioBsL4KqOhg6C3JXKlAuM="},
    /* Go Daddy Root Certificate Authority - G2, sha256 45140b3247eb9cc8 */
    {{0x1a, 0x40, 0x35, 0x39, 0x73, 0xa4, 0x02, 0x45},
     {0x2f, 0xca, 0x02, 0xe6, 0x7e, 0x00, 0x6a, 0x6e},
     3, 65537, 256,
     "v3FiCPH6WTT3G8kYo/eASVjpIoMTpsUgQwE7hPHmhUmfJ+r2hBtOoLTbcJjHMgGx"
     "BT4HTu70+k8vWTAi56sZVmvigAf88xZ1gDlRe+X5NbZ0TqmNghPktj+pA4P6or6K"
     "FWp/3gvDthkUBcrqw6gElDtGfDIN8wBmIsiNaW02jBEYt9OyHGC0OPoCjM7T3UYH"
     "3go+6118yHz7sCtTpJJiaVElBWEaRIGMLKlDliPfrDqBmg4pxRyp6V0etp6eMAo5"
     "zvGIgPtLXcwy7IViQyU0AlYnAZG0O3AqP26x6JyIAX2f1PnbU21gnb8s51iruF9G"
     "/M7EGwM8CetJMVxpRrPgRw=="},
    /* GTS Root R1, sha256 d947432abde7b7fa */
    {{0xb4, 0x22, 0x97, 0x79, 0x89, 0x7b, 0x2b, 0xc6},
     {0xda, 0xb7, 0xf3, 0xd0, 0x72, 0x05, 0xba, 0x3f},
     3, 65537, 512,
     "thECix7joXebO9y/lD63ladAPKH9gvl9MgaCcfb2jH/76Nu8ai6Xl6OMS/kr9rH5"
     "zoQdsfnFl97vufKj6bwSiV6nqlKr+CMny6SxnGPb15l+8Ape62im9MZaRw1NEDPj"
     "TrETo8gYbEvs/AmQ351kKSUjB6G00j0uYODP0gmHu81I8E3CwnqIiru6z1kZ1q+P"
     "sAewnjHxgsHA3y6mbWwZDrXYfiYaRQM9sHmklCitD38m5agI/pboPGiUU+6DOogr"
     "FZYJsuB6jC511pzrp1Zkj5ZPaK49l8KEj8C8QMALXL32h7M1bKwYUH+E4EzNktMg"
     "6TO8UpmvMrUpsyUqtEj5cuHKZPfmghCN6J3Cioj6OGaK/GP5Afl4/Xtcd/p2h/rs"
     "37EOeZVXtL0m79YB0esWCruOC7XFxYpVq9Os6pFLKcwZpDIlTirxZUTQAs6qzkm0"
     "6p98g7BAe+dDq6dso499iYH6TKX/1Y7DzkvgtdizjkXPdsDtQCv9Uw+wp9U7DbGK"
     "ogPeMa3Md+pvez7W35EiEua++tgy/BBjFFFy3l3WFpO9KWgz7zpm7AeKJt8T11dl"
     "eCfeXkkUAKIAf5qoIbapsZWwpbkNFhHax2xIPEDgfg1azVY80ZcFuctL7TlLnMQ/"
     "0lUTbiSw1nH69MG6zO0b9f6BQdgAmD06yK56mDcYBZU="},
    /* GTS Root R3, sha256 34d8a73ee208d9bc */
    {{0x87, 0xb9, 0xc7, 0x23, 0x6f, 0xc7, 0x2f, 0xa7},
     {0xed, 0x4e, 0x3f, 0x1b, 0x41, 0xfe, 0x6a, 0x61},
     2, 0, 96,
     "H08zhzMpiqGE3svHIVhBiepWnStLhcYdTCe8fyZRcm/in9ajysxFFEaLre9+hozs"
     "sX4v/6lxnRiERQRBVW4r6iZ/u5AB40sZuuRUlkUJsdVskUSthBOOmowNgAwy9uAn"},
    /* GTS Root R4, sha256 349dfa4058c5e263 */
    {{0x40, 0x82, 0x81, 0x4c, 0x38, 0xa6, 0x8e, 0x0a},
     {0x15, 0x0d, 0xd1, 0x6b, 0xee, 0x27, 0x0e, 0x25},
     2, 0, 96,
     "83Rzp2iLYK5DuDXFgTB7S0md+8FhzubeRr1r1WEYNa5A3XP3iZEwWus87oV8okB2"
     "O6nGuEfYKueSkWpz6bFyOZ8pn6KY019eWIZlD6GEZQbR3IvJx3PIjGov5cSr0R2K"},
    /* HARICA TLS ECC Root CA 2021, sha256 3f99cc474acfce4d */
    {{0xe5, 0xb4, 0x89, 0x1b, 0xcb, 0xe2, 0x64, 0xf0},
     {0xb8, 0xe5, 0x6e, 0x5d, 0xf8, 0xfc, 0x46, 0xaf},
     2, 0, 96,
     "OAj+saCW0nqsr0k60MDgwzsoqvFybWUAR4iE/Joma6pLumwECoheF/JVh/wwsDTi"
     "NFhXGoRT6TDZqfKWdMNRH1hJMcyYTmARh3XTcpSQT5sQJSqoeC2+kEFYkBVyp6G3"},
    /* HARICA TLS RSA Root CA 2021, sha256 d95d0e8eda79525b */
    {{0x39, 0x6c, 0xc5, 0xa1, 0xc5, 0xad, 0x9a, 0xdb},
     {0xf3, 0x9a, 0x66, 0x3a, 0x15, 0xc3, 0xc4, 0x57},
     3, 65537, 512,
     "i8Lnr2WbBWeWyQ0kudAOZPzO4iQYLIR/d1HLBBE2uF7taXGnnuQlCZdnwUfCz5EW"
     "NmI9OAThUYL/rNK0ad0u7BGjRe5raztMv4yNpB6dEbnpOPl6DgyY4iMd0U5j1Oe4"
     "QUT7a69r2h/TxZGIW6SJktGB5ow5WKDWaUOprZhSWG7bCvtrz2j646ReOkVzmAfq"
     "XwJy3gyls5+uqR23HbP8ilnnbnJlrfUwlCMH84IWSzWYnFO7L8rkWtnHjR38mJn7"
     "LKSCa/AqH44LX3FcXK5CeymJgcsDo5nKiJ4LQAlBM9vmWHr9rplwwFoP1hOGcS92"
     "afyQ3dstbtHym/Uaa55vFYx68EsooCI4gCRsNqQ78jCR83gTz8E/NavxHREjtUMi"
     "ngGStxgC5RHRgtsVAMxhN8EqfJrh0LqzUEbugqydMfj7I+IDAEhwowkmeRVTYPM4"
     "XK046oEAYxS5M17dC9ugRQcaMwn4TbSnAqZp9MJZBYhlhVauS8vg3jx9LRrI6fsf"
     "o2FK1ioTrXdMGhibkQ9Y2AZUxZf4qj8giqaFpnf2pvwc4u5ulDMqg1CECuVPhvhQ"
     "RXgAgetbaOMmjcx7XFH0FCxAvhpgHXpyYR0fYy2Iqs6iRZAI/Gu+s1AqWv2oSBhG"
     "1pBAkpAKhF5oMfjr7Q3THcZ9mRhVVidlLo1FxSTszuM="},
    /* Hellenic Academic and Research Institutions ECC RootCA 2015, sha256 44b545aa8a25e65a */
    {{0xa8, 0xb2, 0xb5, 0xfd, 0x08, 0x10, 0x0c, 0x10},
     {0xd4, 0x31, 0xf9, 0x3b, 0x70, 0x79, 0x83, 0xb5},
     2, 0, 96,
     "kqBB6EuChFzi+DERmYZkTgklL51BLwquNU90lbJRZGuNa+Y/cJXwBURHpnI4UHaV"
     "AlqOriie+S1Ome8sSG9MJSno0XFb3x3BdTe01/p7ekKcagpWWnxpC6qACSRsfsFG"},
    /* Hellenic Academic and Research Institutions RootCA 2015, sha256 a040929a02ce53b4 */
    {{0x4c, 0x3a, 0x28, 0x91, 0x9d, 0xa9, 0x6f, 0x5b},
     {0xdb, 0x12, 0x03, 0x0c, 0x3d, 0x0d, 0x60, 0x5d},
     3, 65537, 512,
     "wvipPxuJ/Dw8BF09kDawkTp5PGZa7205AUkatLfPf00jU7eQAOMTKiimMfGRAOMo"
     "7K4hQc4f2v19ElsBgw+5sF+Z4fISg4BNBj7frK/noYhrMa/wi9AYM7jbRWo09AKA"
     "JCgKAhWVXnYqDZk6FFv2y8tTvBNNAYg3lCUbQrwi2I6jll462TLbPujwEGXtdOEv"
     "p3yvJzS7KX2bts8JyOXTCvyIZWV0CtxzHFzNQLEc1LaEjExQz2iOqFmuwidOgqI1"
     "3RT0H/+yd9WHL6pufSQn58bLJubl/mcHY9hFDd06WWU5WHqSmXI9nIReiCG41fQs"
     "/NlwUk94uL08K4uVmPWz0WjPIBR+TFxf54vl9TWBGTfXEQi3Zr7TSs6DVwA6w4H4"
     "F8uSNl3Ro9h1G+GLJ+p6SEH9RRkGrSeZTsFwR921n4FTEuWxjEhdMUMX44zGemOW"
     "SykwToROYhlePM6XkKV/Aeud4PiLid0lmD2Stn7v2fFRUX0tJshpWWHgrGq4KjYR"
     "BHpQvTKEvi/cctXXHRZH5EdmID/0lsWvjgF6pQ96ZPUNGIfZrojV+oTBOsBpKC3y"
     "DWhRquOld8akkA6hN4sxI0fBCQjrbvd4m9eC/IQgmUkZthJGsftFVRapo2WsnAcP"
     "6mvcHy4GcuyGiBLkLdtfBS/k8APTJjPngMLNQqEXNAs="},
    /* HiPKI Root CA - G1, sha256 f015ce3cc239bfef */
    {{0x12, 0x78, 0xc4, 0x7a, 0x15, 0xcc, 0xad, 0xf0},
     {0xf3, 0xcc, 0x23, 0xe5, 0xc9, 0x0a, 0xb8, 0xf2},
     3, 65537, 512,
     "9B5/UnMyDHPkvRN0o9QwqNCuS9i233VHZvR85zkEHmpwINJaR3JnVfSl6J3VHiGh"
     "8Ge6zCFovkRTv4354twvVcg3Px+kwJyz5HdcoEb+d/oaoDjq7Zpy3iu9lFc6uux5"
     "5199QmQ5eiY29yTw1S+6lZgRZq2XNdZ1AYDgr/SEYYwNHl98h5ZeQa/rh+r4XfEu"
     "iAU+TCK72h8q3VJGZDnzQs7ZngyzsHeXZJzA9KMuH5UHsBffMNsAGJZMoYFL3QRt"
     "U6M9/Aes1MU3guvklQgZKILSQjqj2FPseYlgSGDIcpJQ3AOPgz+yQlda22rpEZfd"
     "hSi8MEyr48KxRURHH+CKFgeW0iEPU8DtqX7UTuybCeyvQqww1r/REEXgphaypcXT"
     "T3OUM3ECoWqj1jOXTyFjHluP2cFeRXF3D4FdXyGarYPM+l7WjSNfGz1BryB1ZlpK"
     "9p/7qxj3ccC2HTHsOyDry+K49a6SsvfhhEvyovKTmiKe0xRvNlS9H15ZFblzqMF8"
     "b3ti6RZsR1pl8w4Rm0bZ/W3c1pzAtH2lsN0/Vm+h+fbkEkj9Bn8SV7apI09bA8Pg"
     "cSojt/ewsTu8mL3WmKgMa/aOEmem8rJY5AIJEzypuxC00jBF8ez3ABHfZfjcK0NV"
     "vxaXxA/VLGGEqnKG/uY6fsI/fe78LxQ+5oXdUG+3Se0="},
    /* Hongkong Post Root CA 3, sha256 5a2fc03f0c83b090 */
    {{0xeb, 0x91, 0x24, 0x4c, 0x66, 0x42, 0x88, 0xc6},
     {0x51, 0x04, 0xe8, 0xee, 0xe5, 0x50, 0x18, 0x3e},
     3, 65537, 512,
     "s4jX6s4PIE6+5tYDbe5Z/MJX3ylooYMOPmjHaFicHGBLiUMMudQVsu7BTnXptafv"
     "5ek1meTMHOdLX40zMCAzU9mmu9U+E47pH4dJrVAtUMoYvgFYohNwlruJiFaAXPi9"
     "LDzhTFeIu9O5le/Lx/baMXQopuZUifVBMcrlJhrNguBw2jspu9UD9Zm6VfVk0WAO"
     "s4lJuIovBdKERSh8j2hQEnj8C7VTy8KYHISjnrC+I6Ta3MgrHtpuRR6JmNr5AC4G"
     "6Qw7cNVQJYiZy81zYPfV/zVnxaG8XqvNSrhF68hoHg0NFEYS49JkYopCmLy0xggI"
     "+P2oTGScdgG9L6lsMw/YPyi4PGkBQoZ+acHJBsrlekZl6cLWUEEuP7fk7WzXvyYB"
     "EaIWKUprNAaQ7BPStvtqdtI87fDWLd3hFeyjmy8syT4r5Gk7/3IlsTaGW8d/a4tV"
     "G0rFIGE9rstQ4Qg6vrCPY0FTMAhZPJgdd7pjkXrKEFBgv/DXvJWHj5fF/pdqAZSj"
     "fFuFHSo5OtBUodE5cZ39Ifm1e/Di4AKPbpYkJSygHiyoxImn7+2ZBi+2CkxP26LM"
     "NxqvR4Util/ENDRMAP0Yk2cT0TfmSLSLBsVXexmGCnnLAMlSr0L/N4/hox56PVCr"
     "YwbnFbU/tkU3lDexfvJIw3/Fdf6XjUWPGqcacigaQA8="},
    /* IdenTrust Commercial Root CA 1, sha256 5d56499be4d2e08b */
    {{0xdd, 0xfc, 0xff, 0xd3, 0x9a, 0xc0, 0x05, 0x36},
     {0x38, 0x24, 0xd6, 0xc5, 0x8a, 0xdf, 0x8c, 0x4b},
     3, 65537, 512,
     "p1AZ3j+ZPdQzRvFvUWGCsqlPj2eJXYTZU90MKNnX8P+ulUNymfm1XXyKwULhMVB0"
     "0YENfM2bIatD4qytXoZu8wmKH1oyvaLrlPnoXArs/5jSr3GztFOfTofvkry97E8y"
     "MIhLF15XxFPC9gKXjdliK78kH2KN38O4KUtJeDyTYIgi/JnaNsjCotQsVABnNW5z"
     "vwJY8KTd5bCiJnrK4DalGRb1/bfvrj9A9W1aBP3ONMok3HQjG10zExJdxAEl9jDd"
     "Al2f4NVHvbTrG6G7SUnYn1sC84rkJJDkYk9Pwa+LDnQXqNFyiGp6AUnMtEZ5xhex"
     "2pgeB1n6dSGFZd2QVs77q6VgncSd+VKwi72H+Y8rIwojdjv3M+HJAPNp+Uui4E68"
     "fpM5hAf3RHB+/gda5bGs0RjM8jXlSUkIylbJPfsPGH2LO8ETwk2PyU8ON+kfoQ5q"
     "32IuyzUGUXksyCU49PpLp4lcnNLjDTmGSnR81VmHwj9ODFxS9D33UoLx6qOs/Uk0"
     "GijzQYg6E+7o3v+ZHV+6y+ge8rlQYMAx03Pl776g7TMLdL4gIMRnbPAIA3pVgH9G"
     "Tpan9B4+4fbYCeEzZCtj1zJen/nAew94b5e8k5r5nBKQeHqAhxXXcnScVXR4sbrh"
     "bnAEuk+gumjDe/8x8HM9PZQqsQtBDqD+TYhla3kztNc="},
    /* IdenTrust Public Sector Root CA 1, sha256 30d0895a9a448a26 */
    {{0x66, 0xc4, 0x5d, 0x9b, 0xf0, 0x88, 0xae, 0xdc},
     {0x40, 0x77, 0x77, 0xa5, 0x54, 0x45, 0x76, 0x4a},
     3, 65537, 512,
     "tiKU/KRIr+hHawr7J3bk8j+KO3pKLDEqjI2wqcMxa6h3doQmtqyBQg0I61VYu3r4"
     "vGV98qBti6hH6WJ2HhHuCBTRskQW9OrQ+h4vXtvLc0GuvACwSitAsqzhO0vCLZ3k"
     "oZvsGjoe8Aiz0OQkNQefnLTJUm3bB8qPtVvwg/NPxy2lyK3LlSCkMShXWFrkjRua"
     "q54NDPIKMzkiOQqXLvNTd7lERf2EyzYggVktmm9tSEhhykzfU9GvUrxEn6sva4Ny"
     "73WA2gYzG13I2mPGTc2sZjHN0d4+hxA24bmkeu9gULLLyqZW4DevqzQTOSXoOWbk"
     "mHqqEpicWWaGPq3xsMo+Bg978BFLN6BEbXvLqIxx9NW1kTbM8BXGK95RF7GXTFA9"
     "sZVZfAV9LSHVAL8BZ6Jee6Zc8vci8ZANk9uqRFFmzH12A+tqqCo4GZd2DWuKYfm8"
     "9u52/XAr3Sk8+AoeW0Ici1YvVRscoS61xxbm+Ko8ko5ptgHBtYadiQ8LOJRU6Orc"
     "nj0lvFMm7dWrOarFQExUq7K02dn413LbHLxtvWVf74g1KmYv7vazZfAzjXyYQWlG"
     "D0McafqbtdBhas3KS9lMkEarFVmhR1QpLoMoXxzCoqtyFwAGjkXsi+IzPX/aGUTk"
     "YnLD3yLG8lbU3V+Vcu1tX/dIA1v9xSqg9nMjhBAbAec="},
    /* ISRG Root X1, sha256 96bcec06264976f3 */
    {{0xf6, 0xdb, 0x2f, 0xbd, 0x9d, 0xd8, 0x5d, 0x92},
     {0x79, 0x5d, 0x91, 0x02, 0xb7, 0xee, 0x9e, 0xc2},
     3, 65537, 512,
     "regkc/QUN/ObnitXKByHvty33ziQjG485legePd1wqL+9Wpu9gBPKNveaIZsRJO2"
     "sWP9FBJrvx/S6jGbIX7RMzy6SPXded+zuP8S8SGaS8GKhnFpSmZmbI9+PHC/rSki"
     "BvPkwOaAruJLj7eZfpQDn9NHl3yZSCNT6DiuTwpvgy7RSVeMgHS22i/QOI17A3Ah"
     "G3XyMDz6j67d2mOr6xZPwo4RS37PC+j/tXcu9LJ7SuBMEiUMcI0DKaDhUyTsE9nu"
     "Gb8Qs0qMP4mjYVHerIcHlPRjcewu4m9bmIHhiVw0eWx27zuQYnnm26SaLybF0BDh"
     "Dt7ZEI4W+7f3qPfH5QIHmI82CJXn4jeWDTZ1nvsOcrEdm7wD+UkF2IHdBbQq1kHp"
     "rAF2lQoP2N/VvRIfNS8oF2zSmMGoCWR3bkc3us6sWV5onX9y1onFBkEpPlk+3Sb1"
     "JMkRp1qjTEAfRqGZtac6UW6GO559cqcSBXhZ7T5ReBULA4+N0C8Fsj57ShxLcwUS"
     "/Mbq4FATfEOTdLPKdOeOHwEI0DDUW3E2tAe6wTAwXEi3gjuYpn1giqKjKYLMur2D"
     "BBuigwNBodYF8RvCtvCofIY7RqhIKojcdpp2vx9qpT0Zj+s482TeyCsNCij/99vi"
     "FULUItAnXeF5/hjncIitTubZizrG3SdRbv+8ZPUzQ08="},
    /* ISRG Root X2, sha256 69729b8e15a86efc */
    {{0x74, 0xd0, 0x32, 0x2c, 0x9c, 0x0b, 0x17, 0x79},
     {0x30, 0xaa, 0x57, 0xb4, 0x66, 0x04, 0x6e, 0xe3},
     2, 0, 96,
     "zZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW+1qLFNvicWDEukWVEYmO6gbf9yoW"
     "HKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9ItgKbppbd9/w+kHsOdx1ymgHDB/q"},
    /* Izenpe.com, sha256 2530cc8e98321502 */
    {{0x91, 0xc9, 0xe0, 0x5a, 0x44, 0xb8, 0x48, 0xc6},
     {0xc2, 0x37, 0x18, 0x14, 0x3c, 0x01, 0xd1, 0xd2},
     3, 65537, 512,
     "ydN6yg8erKeG6BZlarHCG0UycZXZ/hBbzK/npXkBj4nDyvJVcfd3vneU83KkLETY"
     "npKbFDqh5ySQCgpWjsXYJpTh2UjhLT7aCnLdo5kV2oGih/R7biZ3iVit1usMskF6"
     "c25t23p4QekIiBJ+hy5mEWNsVPs8nXLAvC7/wrfdDXbjOtf3tGi+ovXjgW7BRm9d"
     "jeBNxlRViRozMQqxV7mjipjD7Ds0xZVBaX51wjwgxWG6UUegIJCToZBL8058hUVU"
     "mtEFJkGwtU0dM77EA8glfMFw2zv0CS1UJ0isL+HErD7Iy5JMUzk3I+zTAfngCURN"
     "TWTA4Q1ahyK8rRuj/ia1FfOn/IQZ6eyhiLREaYSD84nRdAapzAvWwt4nhVAmyhe4"
     "yXqHViwaAR5svhOtEKy1JPU4kaHWS9rxu9LeR7XxvIH2WWvPGVPpjRXLSsupb0Tl"
     "G0HP4YanytBqn7xMjQYzWqKF5ZA1oGJcFk7w46L6Axq0LHGzWCzeewvbGg/r3iEf"
     "BncGA7DJ75n8wLlPC4Yo/tK56uPapcNHaRLg2/D2GYvte3DXAtbthxgoLAQkTHfk"
     "SIoaxjua1A/K+nXSAUBajXm/i89Lz6oWwZXkrUyKPheR1LFi5YLlgASkA36Nv9p/"
     "og+XTwzTDfvX0eVyfhzId/9bmg+3rgVG5fGoFuxHpBc="},
    /* Microsec e-Szigno Root CA 2009, sha256 3c5f81fea5fab82c */
    {{0x0a, 0x95, 0x8c, 0x56, 0xdb, 0x1e, 0x64, 0x30},
     {0x1a, 0x08, 0x8b, 0x0d, 0xfd, 0xb0, 0xe9, 0x7d},
     3, 65537, 256,
     "6fiP82Ot2obYp+BC+8+R3qYm+JmlY3Ctm67KM0B9bZZuoQ5E7uETnZRCUpq9dYV0"
     "LKgOHZO2GLeMLKjP+1xxudrs/uh+j+QvHbKodYfYt6HlO8+ZSkbQgxl9wKESHJVt"
     "SvTYx6VNMy6FOUB1fhR8gBKYUMdBZ7iggGFUpmxOH+CdDgfpyboz5/7AVSgsAoCn"
     "GfWe3FVTA5d7B0j/mfs3iiTEWcxQEGOOqqkasIQahvlfu7FQbqTRCszVcX4fpxt8"
     "9VNuIl/LK+bUfF2u1sLGTOUFAdntV/zBI3n8+sgkg5XztWpRAdB31ukSofkag/uC"
     "G7mwl/R2BjNDSaD/C7X6tQ=="},
    /* Microsoft ECC Root Certificate Authority 2017, sha256 358df39d764af9e1 */
    {{0x2f, 0xbd, 0x83, 0xfe, 0x17, 0x38, 0x27, 0x7a},
     {0x8c, 0x93, 0xf6, 0x43, 0x08, 0xfc, 0x3b, 0xc3},
     2, 0, 96,
     "1Lw9AkJ1QRMjzYAEhgJRL2qogWILZcz2yp0eb0pmUaID2Z2R+rYWsYxu3nzN23mm"
     "L867znEv5aWrKOxjBGaZ+PrykxAF4YEoQuPGaPTmG4RgSomv7XkPO87x9kT1AXjA"},
    /* Microsoft RSA Root Certificate Authority 2017, sha256 c741f70f4b2a8d88 */
    {{0xdc, 0x53, 0x7d, 0x80, 0xc2, 0xf1, 0xdb, 0x57},
     {0x6f, 0xf9, 0x92, 0x49, 0xdb, 0x7a, 0x74, 0x07},
     3, 65537, 512,
     "ylu+lDOMKZWRFgqVvUdiwYnzmTbfRpDJpe14am9HkWj4J2dQMx2hpvvg5UOjhAJX"
     "AV2cSECCUxC8v8c7aJC2gi3l9GXQzG0ZzJX5e6xKlK0O3ktDHYcHkhOQgINkNTkE"
     "/OXpbLO2H1CUOGVQXBdGubaFtRy1F+jWRZ3YsiawysRwSq5gpN2z2ez8O9VXcrw/"
     "yMmy3ktr+CNsA8AFvZXHzXM7ZoBk4xqsLvlHBfIGtptz9XgzW8eh+ycqobSakYyR"
     "0zqCPnZAtM1SYVFwKD/FxVryyYxJuxRbTcj/Z01MEpat9f54qJeH1/1eIIDcoUsi"
     "+9SJrbrOR5dHVXuPRchnKISVHGgw7+9J4DV7ZOeYsJTaTYU7PlXEKK9X854T20Yn"
     "nx6iXkSDpKXK1ROzSz/E48LmhmGkUjC5eiBPbw84U8szDBMrj9aavSrILbEcfUtR"
     "ykfRSCdyXYfr1UXmSGWdr1KQuluiGGVXEp9oudQVa5TEaSKY9DPg7flRjkFQyTRP"
     "dpCs/DjB2OF7uePjlOFGacsOClBrE7qsDzdatxK1kIEeVq5XIobZydLR11HjqzvG"
     "Vf0eDtN0CtHaquppuJcoj0jEB/hSQzr0ylU1LLCmasCc+fKB4RJqwEXZZ7PO/yOi"
     "iQpU1BS5KqjX7PmrzSVYMnmPkFuYOcQIBsGsfw49AKU="},
    /* NAVER Global Root Certification Authority, sha256 88f438dcf8ffd1fa */
    {{0x95, 0x44, 0xba, 0xa1, 0xdc, 0x71, 0x11, 0x70},
     {0x69, 0xd6, 0xe3, 0x08, 0x6c, 0x19, 0x42, 0x57},
     3, 65537, 512,
     "ttTxk1y1QIkKqw2QW1BjrpCUdBdFctZ7ZVopS6dWoEu4L0J16dl7JFoxZasXF9Ez"
     "OtkR3EA2h9/HaukmXlmKd+PoSJwxFvo+kbHKyaPin84hU6MCNjDLUgLl2jJdw8Xm"
     "+e4Rx4vJRB6EkxhKtJ/lEmRp0CaFYgG2yQIdvoNRu1za+K0Vapn3klT3NFvpv+op"
     "gRLUU5GWs5Fa3f6Qcyj7MEa1yggHx3FyyWbTNJf2jPQYSuHQPVpFtmmnKfsjzojY"
     "EpwASKimD7M7ko1xDnTFi8hM+fSbjrg8ae1vO1AvWO3EsNAcG2oM4rxEqtjNFF2U"
     "eGG/Dm7aKrwvDAtxprMWP5zm+cyfUzXiA6CgGL+78b701oyHDUL3Brnxbe0ElKj+"
     "ttMGxkBh352d81R2zlM6AaaSQewEo48NotUJytbLmvHvQ13Aq6VBz1xTcHDJiKYt"
     "1Gthc1AmhmEOXxvCK+KM1budwQNCupTaX6mwysxNCu9HaQMvIvvxKM6/XVBlqJBt"
     "s3SwCMesqNHrPpz8XRqDLivLtfNEnTqnF2GWonHTcJYVTbdMc+4ZXMVbPkH+rHVg"
     "OxtjzgDd2giQYrTlLe5Ip2sXmVS+h0rjqV4ETOsQbVTW7/Ho8mIWy4Br7T3t9R8w"
     "pa5LyRPtigEBybhRWMBmOrFmS8TVMQJi6XSEDNtNRi0="},
    /* NetLock Arany (Class Gold) Főtanúsítvány, sha256 6c61dac3a2def031 */
    {{0x4f, 0xfb, 0x4a, 0xa4, 0x8f, 0x6d, 0x8c, 0x1a},
     {0xea, 0x37, 0xb0, 0x8b, 0x9f, 0x2d, 0xce, 0x2d},
     3, 43147, 256,
     "xCRec75LbRTDofTjl5Bu0jBFHjzuZ9lk4BqKf8owyoPjIMHj9DrTlF8afFttvzBP"
     "hCf2nx9JvMaZCpDyD/V/Q4Q3Y1GLeqVw/HpYzY6b7cNGbIRwXdrzAZAj/E4wqX7h"
     "J2Pn7WQ8oLjJM2P+FpD/sLj916jAwJRDC7bVWaaeVtAkH3B5r9s5VA1lddkVQZQB"
     "r17s9o3x/61k/iCa11zr/qYfCGSji3ZVrR47KGAuhyXoqq8fxmRGILdwfzzeSNuW"
     "U7c5d+Qa4scWhHaXWy+7GRWF+GmF9ZmnqfI0p6m2pgP8b4Y9VHx2BJtr+UBdADTH"
     "Lpl1neWIA6pN+APSQnbAGw=="},
    /* OISTE Server Root ECC G1, sha256 eec997c0c30f216f */
    {{0x03, 0xe7, 0xf8, 0x94, 0x47, 0x73, 0xe7, 0xff},
     {0x63, 0xeb, 0x16, 0xcd, 0x47, 0x22, 0xce, 0x8b},
     2, 0, 96,
     "Fy/6ErysGPMK9ETWdkKes+gft3mpWLb4ZdE6IU+o66O+pDJy87YBySv9d4VuU92t"
     "6qouJZK26SERqK+1TAvzlmCaO+fqGngutD3lKN4cgLpcbg3bGaXjP5wquEBL3eZP"},
    /* OISTE Server Root RSA G1, sha256 9ae36232a5189ffd */
    {{0xc9, 0x7a, 0x9c, 0x61, 0xec, 0x4d, 0x8e, 0x88},
     {0x61, 0x64, 0xd1, 0x25, 0xf2, 0x63, 0x8c, 0x58},
     3, 65537, 512,
     "qq70q4LP++U3C+fVlq2Q6Esp3FVg48y8s7wtkrnko3rxgbSecnJD3z+rCya07nsa"
     "afso0HJcSm1pmfBjHgzSsf/WjDTQ7tusSLnqsBSO2AepJJjd6Qm+qCIbWTnRJ4fc"
     "HM34+7Pr6ZN47Q/ON3wmd26h2CwhTOSKT8cTPG7H1ROXsqjbJGmDVtNpy4ISu50b"
     "8vg08pgrKo4EZ/bjh6EdrW7ONnQOXjM7y9tRl5RqlTzOGFpuS8b8B48uGrlK92Q0"
     "KdymjVDhjYtL5Ugbbi6AED/knxtlPxG06ldpn7QA64UkmST1IZ2Xqvs0fwJrFZCt"
     "u55aGX+kjNj6bSj8OMfjTK1qztlOk5KOzAxnvwtLls5mZ1NoyxcRjln3rJwbuY5o"
     "RLcYr+blD2XclQmwkxK1Hz6UpceIdSGx3gkkKkzivOxMZ0fCKYi5Crr5wXTOjBgm"
     "Zdr3b8aMe2hcC+5jwF5L8U7Mny8P4eiaepPx4MjbvyfmZSl7NuAzFXPynWuECGgr"
     "NgcrJ8x42GqHDkd09KqgE11kfvTbFK77OuQvwWXjuXpAbPAGt3som9fhXzhzlKzZ"
     "cJMt3ISvRhyieiw/gSZC59TYxWyEZhGLd2tUHKO12BDwrin3ZwiIF1y4l3nP6isq"
     "7lgz5W3pKaplAQ2CE+wlC10sQHIVKdOQLPcaQ9Vq8Gk="},
    /* OISTE WISeKey Global Root GB CA, sha256 6b9c08e86eb0f767 */
    {{0x1f, 0x11, 0x12, 0x49, 0x4a, 0x27, 0x47, 0x94},
     {0x20, 0xaa, 0x0e, 0xc0, 0x10, 0x79, 0xf9, 0x99},
     3, 65537, 256,
     "2Be3HEokKtaXscriHvt9OO+Y9bI5mE4nuBFde9IllIiCFSZqGzG7qFshISvYD06f"
     "WvGxWuR51jIjK+FTzJlFXHtPrby/h0oLS5daqPZI7H17Dc0hBt+eFf1Biki3IPSh"
     "ehtX1F1Q/7pn2COZH8g/497/b1t3sWtuuMlk9+HKQUYOKXHQuSP8yYFfTvdv37+E"
     "rXNku7dCjmn21HYdfp2nuFeKUWdy19SouJVUQHMD9ur06/4oQnc/nSMbsrY9gBQH"
     "TC5P99UKFg29ZkM3fiNDecNAhvVMKdqOmq0NpQSHiB6F4+lT1ZvIiwNjeOvgGUpu"
     "uy9rM2RYk61pv48b74JIxw=="},
    /* OISTE WISeKey Global Root GC CA, sha256 8560f91c3624daba */
    {{0x4f, 0x02, 0xf6, 0xa2, 0xa7, 0x84, 0x6c, 0xe9},
     {0x8b, 0xf9, 0x77, 0x0b, 0xcd, 0xdc, 0xbe, 0x07},
     2, 0, 96,
     "TOlQwMYPchi82PG6s4nieUqjFqdrVCTbUf/q9Akkwwsin8tqJ4KBDdLArzHkdIJu"
     "yiXZjHWd8dvQmqJLIX4Wp2OQ0jnUsYd4XxiWD1AbNTcPasbc2RNNpI6QN+a9WzGR"},
    /* QuoVadis Root CA 1 G3, sha256 8a866fd1b276b57e */
    {{0x15, 0x6f, 0x07, 0x2c, 0x19, 0x45, 0x2a, 0x9f},
     {0xea, 0xdf, 0x32, 0x92, 0x33, 0xc7, 0x9b, 0x78},
     3, 65537, 512,
     "oL5QEI7p8mxAtASchbkxytwt5BGpBDwbVcHnWDAdJLTD74XejCzhwT3fguZPrUeH"
     "bOxbScFK1buP7Iesf4Kahuw9A5lSAdI1nqza8FPJZjzUrAIB2iTTO6gCRq+kHOP4"
     "c1h2t/YOkA218M/M+vnGTOXDhjAKjRd+NevF37sOnMCNh+OIOIVn+j7Hq+ATnAUY"
     "mM+T9bGStPwj08/VxCdJ4J48mwiji10qIeD8OapT2n1+zxoJU7xdBQTPoUqPi3aC"
     "DaH40scUd1uQNgeBmz4G+lJeY8WmAP6l6VIbUrWSOXIDCWK9sGAWbqbdJcIDZt3z"
     "BNFA4k6LhvRv5YOgJ4ReBMH1kL0wPcTvqGm8OJukpJbRYtppwAGWrsvEUTTqDKr/"
     "IY5Zj0pc5GGap9LpKniNUT06Fe6iWY6pXN7F+ZAi5YhFcd2RmWx6nz09mHxe9r4W"
     "aKBergsj/FoPqiJ2LcmhEB3k00QjkIifxirm1/Was1geLzCJCBtUorWYI+wIdxyV"
     "XWHRy4mcX6JKkZrvIapJFgiovWEoMcl0rYX22cWxi9HlEDJNX4sgOjxJHzOFWQ3b"
     "ywl1Q2lz+2txffDfxEx9xqMuyJV5y3Oijk5NJPte5AS+chumJy1JWpl611wJILd/"
     "lLlP8Q0cXohCGxG355Hbnmz0at+MBpgDrcwo76VH81M="},
    /* QuoVadis Root CA 2 G3, sha256 8fe4fb0af93a4d0d */
    {{0xdb, 0x7a, 0xf9, 0x6b, 0xfe, 0x12, 0x04, 0x67},
     {0xff, 0xac, 0xc7, 0x50, 0xbf, 0x14, 0x72, 0x55},
     3, 65537, 512,
     "oa4lsgEY3FeIP0br+a/i6yNx4prRYWYhX6qvJ1HlbhsW1C19ULBTd714OmDiZAKb"
     "fIab1hqOrf8fFX/VlR4Sy+YUhATB3zazFp+K48nbmDTO2DMXKEb8p8nw0rTVTQly"
     "Sfnyh+Op2n2hfWuyOiWpbVJErPi+bvvcpnORkGGmAxQg8ueHo4itraCM/6YLJVIl"
     "5xYB1cu4NYEMozvw4eH8Wl3OgHFt+EmrPju6uNeAAful61uzxV5gKjGgrzfoIDqf"
     "qDIsDMwJHdOejl28TJjuxRpoe+xTpukUNaPfzYCfDEj7HPTxv0q4+tWMcUrHH63+"
     "QZqzg13yhFbvpVdDzimtjKtVv8T7WwHdIyGhWACOw9BqE+0T4xIrgNxn5pWyzR4i"
     "bir4QdTyyhQHjYpVEsZp9biGaC9TXrDSqiHBmOYw42dVx5turBmoVaZFBtAjOtvr"
     "ZV0qERHwO0/KbfQ0xHHk/wBa9lyuI2CFc/HkELElrtWSuxPBDOA52rQ5V7WrNapy"
     "ITuDNecx33ohbrgyCH0dMpEVSmJyz+N3obzVERt2AWcI4EELw+sVbvikGdmiq6/i"
     "J1JWKwKKLBQk+b9CAr8myMaP4G44fVMt5e2Ys5VjaH/5NfTfiMVgNZLAfGkcYZUW"
     "0OveC68+BBBFZVhQOK9I8lm2FvI8DZACxnAuAa08Fdc="},
    /* QuoVadis Root CA 3 G3, sha256 88ef81de202eb018 */
    {{0x81, 0x4a, 0xc9, 0x39, 0xe3, 0xb9, 0x6a, 0x71},
     {0x3a, 0x27, 0x7b, 0x3c, 0xa4, 0xec, 0xee, 0x5e},
     3, 65537, 512,
     "s8sOEGeO6hSXpzIqClY2f2hMx7NvOiMUkf8Zf6XKrO6zdp166Ysbq2sx2/oLU0yv"
     "xaUaeTyKTP+s3yXeTtmCMgtE3srbjKyjbhaDO6ZkSzKJ+xYWOH7rQ+LTdErCYgpz"
     "Ct1Js1fSsAqFnXE83qPLwDLzATkgQxs10VOzse7Fk2mCPha1KEah3uqJCe1DuAVG"
     "iob1WUe+G28BIRC5/anSKMoQOQnKEzbPnK2tQHR5KwI/NP/6IGl90+5h9bqz5zDQ"
     "NyOGcmFFKUhZaG93pi6BvgdNb6/OxEUTkRRwBo8fn/iHabEO78OJGevqHGH8emyK"
     "3NYDC54muhLd1FQ5qyajM+p1gdotzQ9P5APR7xWXG2uQxQKQk2YCIbFH3ouaSoC5"
     "VY+1oi/A1jNn2n7Ep7QEROtH++ZYufcM8HsrscBwKcNAYi07SGncIzxI63sJealt"
     "2qgwmM+AcgOIpltGrnJ5fAgDIWWut+EcpbEqojHeZgT3wHTocd7/PVnMliYSi4WV"
     "Vxqra3ULRD0RKDx7Ybfij2dP5ew8TGCAaVc4HgFbjVXox9/AzHcjNEl1fPaYEest"
     "3u1BLhQFAn/g/iDrNecRrCLOVz3eyTBtEAOFzfH/jBa1wbI+iGxgf5BPlff2La0B"
     "OQcE+nWAfb9JUO3vycR8HOuAftu20N0T/snTnNeyl6k="},
    /* SECOM TLS ECC Root CA 2024, sha256 6ab2ab75f51cb4f4 */
    {{0x57, 0x60, 0xf3, 0x20, 0x87, 0xe7, 0x8c, 0x18},
     {0xcf, 0x47, 0x91, 0x48, 0x76, 0xd0, 0xb6, 0x67},
     2, 0, 96,
     "7NzFMtu9dzQXSNC12fabk0+GlC5finB3R7XaZonRUd20aFiWObtuNBCLUZSfk6QX"
     "AE55BjEXsXQ/NG8yUqicXjsu9ksDK3JZBgCwLOVh6+nwJXTvso/dEj/GUYH5mBdo"},
    /* SECOM TLS RSA Root CA 2024, sha256 1435f225c5d252d7 */
    {{0x67, 0x32, 0x08, 0xc9, 0x77, 0xd3, 0x7c, 0xdb},
     {0x34, 0x72, 0x65, 0x04, 0xab, 0x9a, 0xa3, 0xf8},
     3, 65537, 512,
     "4TjizUwzxbInq8Tx11gaFYNk5fO+34y7TyM4neh0UgL5JIZbJNLTz2x//L/B71+5"
     "m6X6nGIr7d4lFJBGtjO677hXOz93zkcWaUTm3VbOAjBlt4YWxlcccBHXuZ7o3Q+4"
     "R+ormrBdHeJ1CTUEG8ttQbKIl3G7OZYbnH8/pP8cjPub/0kDVNuMzp7xsVRROOis"
     "Qt53fMoJLlYgoebbuMphOqMCtjkJ7R6efEMfLp8UAVi9ZaLRn76ET/CJkk925ndu"
     "uufC4BatS4mnXFmxN0vUXb0ij9B8O/D8gixQEsVSD4GK8FWRPh3bVd/6bzdkHGJj"
     "y21XI0yejVomZUbRrOfNuz0boPGV1pt18fFC39IHQEth3OFqb5NDO3L+A9bNqTgA"
     "yUgRmIn4ucgDc/Ri/Km3V51ueZjy1/yk0qwJVadAVVrCt56iNeXOyEvzJADGgDQ8"
     "E1Pdaqct8Cynz/47ReQM62vFYO08wcQkrjmX/tesiko1V1yyaf6EfPzUFzmaGy9x"
     "vkCwdbm15EdTolOjE0H2Vb5/APDOyCFEokiYGmXTLdAUl0wKZ4IyjkHGzy0jhpaX"
     "EXE/GJcEvI6VzEchjaBL03EJ0h9pG4OqeIOycKvAo3A+TbetyfsrgYyHzU0a7/qU"
     "jGat1AAq1nVljMpKqpinPTsf/d9H39FTUeJL7TpzzjU="},
    /* Sectigo Public Server Authentication Root E46, sha256 c90f26f0fb1b4018 */
    {{0xbf, 0xa8, 0xd3, 0x63, 0x91, 0x56, 0x8a, 0xa3},
     {0x73, 0xdc, 0x72, 0xff, 0x6f, 0xc2, 0xe7, 0xc9},
     2, 0, 96,
     "dvqZqW4g7fnXd+MHO6jbPV846KtVplZP1kjq7H8tqsOyxXnsmWF/EHnHAlr5BDf1"
     "NDUrd85/II9SowCJ7NWnom1b40uSk6CA9QGU3PBoBx7N7v4lUrUgQxwb/usZzkOj"},
    /* Sectigo Public Server Authentication Root R46, sha256 7bb647a62aeeac88 */
    {{0xd7, 0xc1, 0x4e, 0x9b, 0x9f, 0xc7, 0xc9, 0x97},
     {0x32, 0x67, 0x0d, 0x35, 0xa4, 0xa2, 0x89, 0x33},
     3, 65537, 512,
     "k77VNlJ12AEjoBxHQknuY7a3If3EldVIKyZ8FFMQ2nn9K7ctpNQs+uoy3UnCub0P"
     "SD17WphUr55dMXRPB/xQId2kz2hPGxJjbSWZTCqZ80gwYfqBfB6nCErcPiscHxhM"
     "cao1jK34bug7StnllALWiYQTqm3ITzPMUJY3kjPcX4jnn1TZSPCYQ9Zm/Z8XOEPF"
     "AVEL1+MjDxRdWxTnS77d9MjaAzfR1jmhIVEwg7Bt1zBOlluR8HAkq79FgWRDDb0h"
     "Oi886Z4NyyC1QifM2m+b7mQwkDnNk2WBITG1I1AzNyLjOO34MTDMRf5i+dFdMnlC"
     "h99qzFYZQE3Oqrv5tXZJlPEn+JGlg+UGs2MOgNzgElWApjtmtDmHLcjw0NEU6eQN"
     "TQ72XVdyxTscR1ad4tX7gWGMzE2AkDRbt9cUddzYBEifwMEoiLTpHMqnsfFWt3tJ"
     "TFnlIBWohAIp+jiUaZpJBo/NH3kUFxIMg3reH7GX7vmXeCikyESS6X0mBaZYcpt5"
     "E9gRX67FOGI0aLKGMI74kGGeMmz1BzbNokxu7Io27fLmmRVEcMN8vJw5wLTha/eD"
     "JSNX2RKA5UnwdQ/vjescm1QotCE8/HwK/+97a3X/ix2gGQWr+vgrgULoOLq7+6r9"
     "PeDzyt9Ol5cp7fMYVumllqy9w5CYsuD5otSmR0N8bc8="},
    /* SecureSign Root CA14, sha256 4b009c1034494f9a */
    {{0xbf, 0x84, 0xf8, 0x5f, 0x49, 0x4a, 0x13, 0xce},
     {0x77, 0xe5, 0x7a, 0xcc, 0x3b, 0xb5, 0xc8, 0x5f},
     3, 65537, 512,
     "xdJ6odaKvxYx0JjROpT8WrhuIsFi96cKJ+9Q9i6xnmgS8GwkYznx8N8Qxt63UiDV"
     "UltCmZ7zoL5SH1/MZ22nLlCiwZeNtviV9bC63J3gvsvf9zjyR/WmmpKVKmJZUAui"
     "sTXnZbJhsuqScWnkKfBPgYEEPLKlW9TFqFlne1UcSat6ncLnc03vzQnCxFcS2wEO"
     "I3kJBzui6PyKz4/ARiScOCfgg50boL94FRDrhk4KWv3f2iyCfu7K9inh+nGh94ho"
     "nJzwjb4PSZHY6jr5/dBocdvptStOgpJvZh/g8NxM7MrR6rp0BvmzhJCU0V+OcxkQ"
     "XQLlcKXAENAQfG/FWEm0sG6a2n2V9czaAq+4LH15j75D8fkoKI0JQ/gI3WvIiywk"
     "sY1SB714m8vKaLKk3QxMeWDGmdGT8TAaB9OuIsLqzvGECczgFG5/P37SgoWs3KkW"
     "ToWgYMv2nNfIs47txpuYdQ1V6F/llYsCpK5DKSgRpOYSMAFLdWseZp15L6V2Lx1A"
     "tG3JfXkI7NFqtl0qsqVmvWuF9HRWw/XndVIoLKX/Zkel1P7+nlS/ZX4B1jCPpTac"
     "olAc7jiAAUjGx3T0xqzDQEkWYXQsr4xvNe17GABbNjycUA3KkjMQ8SZJbd91JDeC"
     "Itfolv0VSwKWPgdylX6rPUwu18rw3+BYPy0vBJo4owE="},
    /* SecureSign Root CA15, sha256 e778f0f095fe8437 */
    {{0x3e, 0xc3, 0xb0, 0xbe, 0xab, 0xb9, 0x8c, 0xc6},
     {0xec, 0xf4, 0x37, 0x71, 0x99, 0x5d, 0x3b, 0x54},
     2, 0, 96,
     "C1B0jWQymZmz0mAIuCKORnQseMArRC1tXx3JrktSIIM9uBRtU4dgnl9shdsGFJXg"
     "xyj/nV/kqvGzi23tTy9LyUqUkWR1/gHswdjrepR4VhhDX2uBy/a82rQMtimTCGmP"},
    /* Security Communication ECC RootCA1, sha256 e74fbda55bd564c4 */
    {{0x3f, 0x76, 0x9f, 0xe0, 0xd0, 0xdb, 0x91, 0xee},
     {0x29, 0x67, 0x8a, 0x11, 0x3d, 0x65, 0xcd, 0x2b},
     2, 0, 96,
     "pKVvYAMDw70x9NMXnCuEdazl/T1Xbtdjv+YEiZKOgZzj6UduypASyBPgp533ZXQf"
     "bBCy6OTp722FMplEsV79zHYQ2Fu9osb51kLkV3bckMI1qUuIPBJHbVz/SU8aSlCx"},
    /* Security Communication RootCA2, sha256 513b2cecb810d4cd */
    {{0xb4, 0x6c, 0x4c, 0x6a, 0x33, 0x05, 0x8d, 0xb6},
     {0xee, 0x6b, 0x26, 0xc9, 0xdc, 0xbb, 0x67, 0x45},
     3, 65537, 256,
     "0BU5UrFSs7rFWYLEXVKuOkNlgEvH8pa82zaX1qZkjKhe8OMKHPfflz1LrvZd7CG1"
     "QavNuX52n775PjY0oDvB9jERRXSTPVeAxfmJmcrlq2rUtdpBkBDB1tZCicK/9DgS"
     "lUxUBfc25EWDexRl1twMTdHefgyrO8QVvjpWplpvdmlSqXq5yOtqml1S0C0KazUW"
     "CRCE0GrKOgYAN0fkfldPP4vrZ7iIqsW+U1WykcR9ubCFGQZ4LtthGvqF9UqRoecW"
     "1Y6iOd+UuHAfKD+L/EBeY4M8gyoamWvP3llqO/xvFtcf/UoQ606CFjqsJwxT8a3V"
     "JLBrA1DBLTwW3UQ0Jxp1+w=="},
    /* SSL.com EV Root Certification Authority ECC, sha256 22a2c1f7bded704c */
    {{0x88, 0x1d, 0x69, 0x62, 0xec, 0x6c, 0x6a, 0xdb},
     {0xaf, 0x42, 0xdd, 0x1c, 0xba, 0x29, 0x87, 0x1c},
     2, 0, 96,
     "qhJHkJgb++/DQAeDIE7xMIKiBtHykoZh8vYhaMoAxMfqQwBUhtz9H98AuEFiXNxw"
     "FjLeH5nUzMUHyAgfYRYHUT19XAdT4zU4jN/Nn9kuDUq2GS5acFoG7b7wobDK0Akp"},
    /* SSL.com EV Root Certification Authority RSA R2, sha256 2e7bf16cc22485a7 */
    {{0x1b, 0xdc, 0x46, 0x79, 0x85, 0x43, 0x62, 0xad},
     {0x29, 0x28, 0xdd, 0x9e, 0x3b, 0x71, 0x5b, 0x2a},
     3, 65537, 512,
     "jzZlQOHWTcDXtOlG2mvqM0fNTPl9fb69LT3w23jhhqXZuglXaO1XPqDQCEGD5yhB"
     "JB/jchXQARr7XnAjssufOePPxU7Gkm0mxnu7s9onnQqG6YE3Bf7wcXHswxzpY6IX"
     "FJ3vG2fThVUCAtZJycxa4bH3bzKfydQ7iEGonL3Lq9ttewkfokxykNorCPzPPFTO"
     "Zw+oz12WGQvE43LrrdF9HSfvkusQv1vrO6/PgN3B0pYEW3p+pKk8OHakYo6gOV7q"
     "d89dAFmPZiw+B6KjBSYRaZfqhbcPlgtLyEDhULouisv3D5oi53+aNxPN8k0TayHR"
     "wMwi8qFG9kRpnMphNQcAb9ZhCBHqurj26bNg5U257J8UZslXWNvNh2n4ioYSA0e/"
     "ZhN2rHd9NCSFg83XqpyQGp8hLH94t2S42Oim9HizVcuE0jLEeK6jj2HdzghTreyI"
     "/BXkmg3mnxp3zkyPuBQVPWKchjgGAGYS5Fl2WlPAApiiECtoRHuOec4zSnaqW4EW"
     "G7WK2NAAe15itAnWhmMOpgWVSbooi4iTsjQc2KRVbrcc0N6ZVTsj9CLg+SlmJuwg"
     "UHfbSguPvuUCYHBBXtSuUDkiFCbLsjtzdFVHB3mBOagwE0TlBIqulhMlQg+5U8Sb"
     "/M3kHN48+qvWBkofZ6aYMBzdLNvcGJVXZsb/XItW9Xc="},
    /* SSL.com Root Certification Authority ECC, sha256 3417bb06cc6007da */
    {{0x10, 0xf2, 0x40, 0xf1, 0xe3, 0x4e, 0x47, 0xff},
     {0xb9, 0x4d, 0x73, 0xd6, 0x29, 0xb7, 0x9f, 0x27},
     2, 0, 96,
     "RW6pUMSmIzaeXyiNF8uWImQ/3HqOHcwIs6JxJLqOSbkEG0eWWKstlcjtngg1yCfr"
     "iYxTWOtiiv7wWw9rMVJjQTuJzezsto0Z0zQH3LvGBn/CRZXsy3+oI+AJ6YH680fT"},
    /* SSL.com Root Certification Authority RSA, sha256 85666a562ee0be5c */
    {{0xfe, 0x10, 0xd7, 0x40, 0xf2, 0xf9, 0x3b, 0xb4},
     {0x7d, 0x5e, 0x4a, 0x70, 0x38, 0x07, 0xdf, 0x15},
     3, 65537, 512,
     "+Q/doyt9y9Aq/uxnhabnLhu6d+Hj9a+k7PpKXZHEV0drGHdrdvL9k+Q9D8IWngtm"
     "w1aUnheDhc5W7/IW/QBi9SIJVOhlF05BueBPRpeqG8i4bmJeabFf2yoCfvxsyvNB"
     "2O3Q6Pw/YUjtsAMUHRAOSxngu07shmX/NvNeZwILnYZVYf16OO3+4hkAt2+hUGJ1"
     "dDyg+sglkrRueiLH+B6h47LdkTGrKx0E/6VKBDfphaQzK/3i1lU0fBmkSmjHsqjT"
     "t8qhk4jrwZe8jPkd2SKEJHTHBD1qqSmTzOu4W+H+XyWqNFjIwSNUnRuYEcM4nH49"
     "hmylD0CGfAL0XAJPKMuucZ8POsgz/hElNer8usVgPdl8GNWyqdN1eANyIso6wx/v"
     "LOUuqfqeLLZRRv2vA9bqYGjqhRY2a4XpHsCz3cQk3IAqgUFtlD7I4MmBQQCeXr9/"
     "xQiYohgsQkCz+W84J0tOgPQ9gUfgiHzqHM61dVxRLhwrfxpyKOcAtdF0xtfkn60H"
     "k7ZTNTX8N+TD9l0WviFz3pIK+KBjaryWkmo++LxlVZve9Q2JJgT8JRqmJWnLwm3K"
     "fOJZX5es6+8uyLzXG1k8K8zyGciTaydjGc/86Sb4ynGbf5P+NGeETpnr/LN4CTNw"
     "umamdu0bc+sapQ3EIhMglFYKTixsTrH9z5wJuqIz7Yc="},
    /* SSL.com TLS ECC Root CA 2022, sha256 c32ffd9f46f936d1 */
    {{0xdf, 0xeb, 0x54, 0x9a, 0xa2, 0x92, 0x40, 0xc9},
     {0xb3, 0xf1, 0xc0, 0x2a, 0xe8, 0x88, 0x7c, 0x70},
     2, 0, 96,
     "RSk1c/rCuCPOFH2osU2gWzbuKixTw2AJNbIkZiZpwLOV1l2SQBkOxqUTcPTvElEo"
     "XefMvfk8hcHPlJDJK86SQlhZZ/2UJxBkjE8EsU1J5HtPm/XnCPgDiPenw5JLGVSB"},
    /* SSL.com TLS RSA Root CA 2022, sha256 8faf7d2e2cb4709b */
    {{0x8a, 0xc8, 0x31, 0xb8, 0xce, 0xd6, 0xd8, 0x72},
     {0xa4, 0x93, 0x44, 0x54, 0x9e, 0x9e, 0x5a, 0x28},
     3, 65537, 512,
     "0KQJck9AiBJhPjUjnu72dM8ve1g9zjwNECiQL5f3jEjYoNglsUywEUwXc1DQIkpj"
     "u4HTKW7VtQk+Jhh/shJ/k5i3r/A2v/LuGJ6cO1LFRxlddPNkZtVdx2i0vxscBqO8"
     "j0Ajth7GhL1RxBs5wZXSKexLrnstvzn9tGLelntBxpyg4AZy+/AHlwk5gXSv9zRZ"
     "EVcKwlvBJPQxczCCxp26Avc+fERfgw3z8d0gaRYJUOLUVbbggHJ2bkxHt3VVWbRT"
     "dNmUxkGtWIoxZg8eohspQE4v33vmFiwt/L/s87T6vhj2m0nU7gVu2TTznPHsAYvR"
     "IMYPoLW8F05Ie1HC/OlcaTdHZrNo+BUo8LnTpBXMWk+6UnCjEkXdxrpO+8LQ96hS"
     "J21uebWM/HuMwRZM7oB/vvB2vkFTEjOuWjhCq9cPPkGNdgcy1auJ9k5n2bFCdSNu"
     "881CsvxV9VOHFzvAM1jxUtL5gKTw6PA7izjMpMaQfw+c/YvRo8/ag6dpyVA21VwF"
     "0gpBdNtjETfBpaCWSx6MFhJ3rpQ0ex5/wmYA5KqD6oqQrc42RE3RUem8H/NqBf3A"
     "dB8lGUBRbuqCUUDfm7kIKgYC1SMcE9bp29vGsHrLeyeb++DVRiTtEEtjS6UFj7q4"
     "HSum+pHiklK97Otnl22aLZ+BMgVnMvtICD/ZJbgEJS8="},
    /* Starfield Root Certificate Authority - G2, sha256 2ce1cb0bf9d2f9e1 */
    {{0x42, 0x8f, 0x14, 0xa7, 0x69, 0x61, 0xb8, 0xc6},
     {0xf7, 0x39, 0x95, 0x2d, 0x7d, 0x67, 0x31, 0x19},
     3, 65537, 256,
     "ve3BA/z2j/wCsW9bn0jZnXniorcDYVYYw0e218o9NS6JQ/ehaZveihr9EyCctEl3"
     "MilW/bnsjN0i+nLcJ2GX7vZahOxuGbmJLNyEW9V0+2tfxYmlEFKJRlX0uHUc5n/k"
     "VK5L+FVyVwIZ+BdxWeseKAd0xZ1Ivmy09KSw82Q3eZLA7EZef+FtU0xir80fC2O7"
     "Op37/HkAmGF0zyaCQGPzsnJqGQ2ZytQOdcw3+4uJwVnxYn9fs19lMPint012Wh52"
     "XjTA6JZWmYqz8H+kzb3cMjF8kc/gXxH4a6pJXNGZlNGi42NbCXa1VmLhS3QdltQm"
     "1AgEWdCYDg7m3vzD7B+Q8Q=="},
    /* Starfield Services Root Certificate Authority - G2, sha256 568d6905a2c88708 */
    {{0xe0, 0xe7, 0xc9, 0x56, 0x16, 0x48, 0x17, 0x7c},
     {0xf1, 0x34, 0xb1, 0x6f, 0x3e, 0x15, 0xed, 0x96},
     3, 65537, 256,
     "1Qw6xCr5TuL1vhmXX46IU7EfP8vPnyATbSk6yA99PPdrdjhj2TZgqJteXACAsi9Z"
     "f/aH+SVDhudpG1KakOFx49gtDU5v9shJ2bbzGlauK7Z0FOvP+ybjGrodli5qO1iU"
     "iUdW/yWgk3BTg9qEdBTDZ54EaDrfjkBaHUpOz0ORO+dW1gBwy1Lue32uOue8MflF"
     "9sJgzxNZAiuAzDRH37nekGVtAs8skaam596FGEl8Zk6jOm2pte40LroNA7gz30fr"
     "sWuNJdmbzoHRRUYylnCH3gIOSUOFtmxzu2TqYUGsydRU34cvxyKyJsyfWVRon/y+"
     "Ki/EVRx1QGAXhQJVOYt/BQ=="},
    /* SwissSign RSA TLS Root CA 2022 - 1, sha256 193144f431e0fddb */
    {{0x53, 0x65, 0xe7, 0xb7, 0x42, 0xf5, 0x0f, 0xc2},
     {0xf2, 0xfa, 0xcd, 0xcd, 0x89, 0xdf, 0x2c, 0x7f},
     3, 65537, 512,
     "yypo4gvDV7w1Y7xwpTvzjDxOV5Zuw042pPYCyh6qrrjeqK8ddtq6NdCRcAffswby"
     "ivIuVVF7uywky3+SJoCjtJT2gqGk6Pp1HVnzB2phZOLGjJWvo7uOb1bPccxegWEN"
     "bfKrAi6kl+Vx/IqwkSBbnHRSba4VJ1l48gnKZQ5/y/Tr59ypTHf2KxYEla6ccaU/"
     "KtpBQuc8hBD04T2Ma+IrkUdVT7hWvkXeIlFNTijZXxlBBo8OTQbgcEAjAWrkyxOb"
     "c6xNFEiSLf5tp/iHa3l14b4QsaqIQFlU18/E0JtEszhpZIyB0SN+qjk8Ow+fSnuC"
     "ymtvyiI+MdCw0CockoqP2BmcR+Q+DLnCzb5BDPikRwXbwRcwODpp3M3DaSP9mg8C"
     "zhBqzsr4uSmjNomGrgvAT2O5BllJXg7BabMK83d+Lp2MsyeY0pmNJacfhrOmVHA4"
     "/H1d6E+DDNGT5RLkVNo+8q063j48RfAoDwa54duXe5lFnt3+lVkEL3U/066JmYas"
     "FLSohPrIXTsbWJPBF5RVyAvjgnmEn/MAhDTu3DHVj/L6T5ZMBqp4+95koiPNHz7F"
     "jLw3VA67WnJV78hbtXL4eN83IExXkXOSc6wYd0OCIGns6awpRuULTvg3c4mWihxt"
     "ve++2Lb0ysD9R/CuC1ggxcgdNq6XjVCDJiQp9507DwU="},
    /* SZAFIR ROOT CA2, sha256 a1339d33281a0b56 */
    {{0x86, 0x27, 0xdb, 0xe3, 0xaa, 0x03, 0x61, 0x68},
     {0xa6, 0x90, 0x52, 0xc2, 0xe3, 0xd7, 0x6b, 0xb4},
     3, 65537, 256,
     "t7w+UKhLzUC1zmHnlsq0odoMIrD6tXt2AHeMC899qIbMJlHkID2FDNZY4+f0Khid"
     "2tGuJu7rU9z0kNYTSgyQPMP02tKODZI63LGx/zjew7otX4C5Ar1KnRsPtMPCwWcD"
     "3dwbnD2zsN4AHqg0R7ua6/4LFL02hNoNIL/6W8upFiCtOWDuL3W255ec+T79fk1v"
     "TS/viA1q+t3xPW4gpaAStE1wuc7XcjuJk6eAhBwnSXJJtf87lZ7BzMgB7OgOigqW"
     "57Omh+XW+QUrDZdAcDy6rHVanNVNnQIK0kubZktGBxdlrZ9siADcIong4WTUZ7wx"
     "eWE8u8pBzVxqAMg8OI5Yrw=="},
    /* T-TeleSec GlobalRoot Class 2, sha256 91e2f5788d5810eb */
    {{0x2c, 0xb3, 0x3e, 0x9f, 0xb6, 0xe1, 0x42, 0x49},
     {0xc6, 0xba, 0x63, 0xc9, 0xfd, 0x89, 0x42, 0xc7},
     3, 65537, 256,
     "ql/aG1/oc5Hl2lz0ouZH5fNoVWAFHQKks5tZ8x6KrzSt/A3C2UgZ7mmPySD8IaoH"
     "Ge2wXKxlx1/tAnx7fC0b1rq5gMIYghaE+mawCMZUI4HkzblJP/ZPbjdIKDgPxb7n"
     "aHD9OZdN0seYkVCqxESzI305R+lSYtYSk163MZZCBft2px6j9cL86XrFbKlxT+rL"
     "eLxgr8fe9NnLvn4zpW6Ug/A0+iGr6o5yoD+k3jBb74ZNapVbQ0SoEBUc5QFXxZjx"
     "5gYokaogxbdTJlFDsgsRlVjhwA922cCNfIHzcnCeb/4ajtlfNcaybzR8vkhP4lo5"
     "19ideJ6fhj4DXhmLRKLVxw=="},
    /* T-TeleSec GlobalRoot Class 3, sha256 fd73dad31c644ff1 */
    {{0x20, 0x05, 0xea, 0x79, 0x78, 0x53, 0x35, 0x61},
     {0x31, 0x90, 0xf2, 0x73, 0x83, 0x20, 0x55, 0x9f},
     3, 65537, 256,
     "vXWT8GIibySu4Hp2rH292STVuLf8zfBC4Ot4iFZem5pUHU0MivbTz3D0UrXYkwTj"
     "RoZxQUor8CosVQPWSMPgOTjt8lw8P0S8kz1hq07NDb7wICdYDkR/BBqHpdeWFDaQ"
     "0El7oXX7GmtzsfjOqQks8lPVwxREuIal9osrOdqjM1TZ+nIa9yIVHIiRa39m5cNq"
     "gLAk89+GRYj9GX91hx8fsRsKcyRbuWXgLFTIYNNmFz/hzFQzc5ECOqZ/e3Y5oh+W"
     "tjiutciTdB2eubTlYJ0vVtHg615bTBJwDGxEIKsR2PQZ9tKcUjfn+rbCMTtK1BSZ"
     "rcca9V1f+ge4fA0f1oMesw=="},
    /* Telekom Security TLS ECC Root 2020, sha256 578af4ded0853f4e */
    {{0x0a, 0x03, 0x3f, 0x1d, 0x89, 0x55, 0x49, 0x91},
     {0x56, 0xfb, 0x44, 0xd2, 0xa7, 0xb8, 0x68, 0xe4},
     2, 0, 96,
     "zr/+V6i/1ar3EJrNvNERor1nQsyQ6xUYkNmizQwqJes+T8610o8P8zXaQ4sCgL5v"
     "USQdD2sryp/Cb1Ay5TcgtiD/iA0PbUm72wakh5CSlPQJ0M9/yIALwZezuzUnycIb"},
    /* Telekom Security TLS RSA Root 2023, sha256 efc65cadbb59adb6 */
    {{0xe8, 0x7d, 0x86, 0xbd, 0x7f, 0xef, 0x76, 0x09},
     {0xca, 0xc6, 0x04, 0x48, 0x1c, 0xf4, 0x07, 0x89},
     3, 65537, 512,
     "7TWhgYDzy0ppW8L7UYOuJv3hbvOBEn1xQP+HdUIpIe2BUizfEsEZhInBvcUo1dVL"
     "bETWTNsHlkpVeso2ggQ2qKX8J/ZJ8dVynpH5I9Zwe7v1m8Hsk88Z6mV+iHCgc/z2"
     "/7VWYuFzajSYPoK4rJVT9AGgJwdyowBToOSyq4M4VzMllJ++SB2Y4aO6nlzNBHFR"
     "fXV4q/NZqsTgYL6Pg1K4dRpBNe288zpj6akURdfmUtFu0t684/ULO+bgxL1DZBOm"
     "zvSYN2yKlaiXyEcP8F4Qi+cdHP6xO6AFM2gFQYLBAysByOePTavotfbNa0S1592L"
     "7OoltAAiV02wsbIxwRbO//0UhLdH+rLxcN7bi2w2WKR8sxHRw3d/X7Yl4A3F0rP5"
     "uLh32zdxcUfjYBhPJLZ1N3i5o2Kvvclyji/Mu67b5BVSGQcz+2q3LUuQKIJz/hiL"
     "NY3bpwRqvurBTTY7FjaRMu+2QImRQ+DyoqsELubyTA4WNCCsh8EtfslmRxcUEaTz"
     "96EkiavYGsihXLGj94xtyAHJT8nsxPysUTPRyIPRyZ8d1Ec0KT7LsA76gwsoWOUp"
     "3D98qJ/Jtgq7puhGFg+W5XvkanpIbXaYBaXcbR5CHkLaGuBS97WDwBp7eDUsOPUf"
     "/UmjLtJZY7+AsIyTc8s1ppmVImFlA2D7L5NL+pqcgDs="},
    /* Telia EC TLS Root CA v3, sha256 098e08a91dbbf774 */
    {{0x6d, 0x0e, 0x94, 0xf9, 0xf1, 0xd5, 0x71, 0x99},
     {0xf8, 0x38, 0x6d, 0x42, 0xa9, 0x98, 0x94, 0xd2},
     2, 0, 96,
     "wciWFUMtuYUpSlbSIrh2mvJPp6Zg55LfUk/BadY+aRZGsCRNs9fjSxB8cj2alHtF"
     "uS27eeCDpb4EFJdJ5yG0wKcGZZeP8RpWWZXlxjWMPYehN+EFDcBpykI0wskrg2ll"},
    /* Telia Root CA v2, sha256 242b69742fcb1e5b */
    {{0x36, 0x6a, 0xa8, 0x3b, 0x13, 0x25, 0x71, 0xcf},
     {0xac, 0x33, 0xba, 0xf5, 0x6c, 0x59, 0xc9, 0x1c},
     3, 65537, 512,
     "stA/B7zie9Brmfjid2nnzp2kA7yCbaH+gWUfTCesjgC6FnvrMGoAwLN0aH6yr8fV"
     "YrN6P1DKjDZEJGPSNukMhfZDdtVMoWByZ+IoM6XLMbg6IiM0uH29ViJAner0ewOt"
     "aPyygU+Y0HTqjeV9zWPDo/beksJYGeCWu8XEqT2ldJb+r/mJqr2VF1TYeETxDHcV"
     "kuCYQqek1qogks3BoLOWsjqEQo191ZXk1tvpYsRYs3nFjNM1M4OfdaFSJ2E48Vk9"
     "jlDgvXk852yW/l7ZAmW0jlzQETTfXb9Sp4EAw3+ZRZkV1RfIClPsY/OZfcxpEobC"
     "F/ABnr+EvNFSyxuSZs6kU+Whv8TbCdbmiVYryON83uP/ieU1bijobAsjUaklBetI"
     "+N2xyvpsCFHvtxhsRMom4XPGiQaB5YqssOIpxrkks2tEEfSlQ8JMQ+VwNoy2M1d6"
     "lS6CoPRcELNhg/YCBYYufC1s3ANGbjWT1XqVL94g2Ft+lJAEarpZPQQFdZ03og4u"
     "PevBpFKD/tBr1GaO3MbpEk4dKleqELx8XoJ9pqbJ8i259RcnrdEOiVQrlfrArR2Y"
     "FHgzQoYKqXO1+3QNtxswGcRaDhwnt9oY0P+KyAW68aocoje35kikRiyU6qh2YkeL"
     "EFMHSFds4pJNtq4Fy9zBSl6PrD0ZTsLtYHUr28HKQtU="},
    /* Telia RSA TLS Root CA v3, sha256 d13db1294c45ebc6 */
    {{0xb1, 0xfc, 0x5f, 0xa1, 0x4a, 0x7f, 0x98, 0x4b},
     {0xbe, 0x71, 0xcd, 0xec, 0x5c, 0x7d, 0x04, 0x4e},
     3, 65537, 512,
     "sV89KG19hCf4S1Fvk8D3TyDERhmcvx8F7Kmb4WATx3ije1id3KHxRE0TKmcNCbAQ"
     "57bvHFEYa4hR2l20VjVadExqOW+2ld99MbEiO+jRVOz+BbxLxJnmGwCqI+BfuTjj"
     "VReDxsxjQvjgBsClaO/sm5i70nlZcWGRtIkvWDK3NNkT5RtwXc/O8NTFVpbUqT6c"
     "RjIj3olAblR+lRf4Ffy5o+Q9fabjYn9Z9S4itruElcEFf9Ljk7fwdTycT/rvJW9w"
     "/B3G2a3r0f/zXNOVruIBcqE6pkSospACU2bG42fYKrbM/GWnp7u+p9Frz4jaNwpb"
     "4YHuEeS8BratNcP8X62jXIvvKHxlsMDJCnb4U8JzFOLsU6mohVY58BdZrvi0Gk9U"
     "OuqmgoG6dskHoksjZTlK61D/InzmEoA1yAYJFDVysjRxDUOu9cAwANbqmq77WIFL"
     "6BpnZgVqPtMfG6wN8BrTKdapvilVsYR59BFgIsAVBMxrGh+W+QcvmJafUpASvlAr"
     "KvVG2FI4i6PiLjSBT0+6F6EQLrYqefOQF/fBNEXb+njUQ0SUVrAqtH4Y+OjCI/a4"
     "/JJQppxeemZcQ0SUShgiI5AM5xHO5iyaUrTjYH4zxUz9j+1FEbDH/xpstr1gXByk"
     "spup+hRTaJcbA+UbpJqtWZndAPddJmt6YJQ+dU3pDu8="},
    /* TrustAsia Global Root CA G3, sha256 e0d3226aeb1163c2 */
    {{0x3a, 0xae, 0xab, 0x24, 0xe5, 0x80, 0x48, 0xfb},
     {0xdf, 0xab, 0x58, 0x00, 0x93, 0x69, 0x2b, 0xbf},
     3, 65537, 512,
     "wDGCYZLklBsKKmXQvgaphztREupwQa7i+3TqCo25s0zcj7cTUk9UGOEsc5WRxWY7"
     "as+sY22HU/D38Tm3oENjsMQDXVep50TOxKGDZfZQPrF+Frg6igLQlh8AzQUh7wZt"
     "3SGcGUNFocXogMrCrUBiFwbGqrzz1ub8UH5mQh88i6Z5eYZANZ8g7z/ri0cfj47F"
     "1I62LMlEBOPUQ3U/1T+vHMx+Rl+s32QQiu9G8JDwDy30iAuxKaqvhapJWKi/Y6A4"
     "keaz5ndoxPkqGYS7DuH1r4nspS9QIHQeEkFzHiTZys4soVk1wMgdRidhWo/5TdNy"
     "eWYenxWQIS397YtWcANKST5/aTESacceXMp6E4vo5vVgD8yTLIR/8fxq/JtHndut"
     "iD3zdnUz10ukyIv59UNYT8vIA1SPpYV4BBrzc/LXhx1Bn+fYF84anA9K/NxEaFRo"
     "4kE8/iyEhjc8zT8votvn91QDX1nT95F4x4t3ahblSYWQRXJwL5Fd+D5lQAsZmckm"
     "IFpowTW/T6dR8dgRK1vgmp4oOwo6Ch/BgeUu8Ka5aaWIlOZrE3/RZD89nHBG5aKF"
     "e1iEJ9zEgD5nmprHmjEOMOzmF0CV2UXtAZaqvwzzS9Fj9xNYwLjz+mfdm31tSv8y"
     "TLUlO/8cZw+FIlkFkZFBd4HQhUyHEHH/nkMbrpV1LYE="},
    /* TrustAsia Global Root CA G4, sha256 be4b56cb5056c013 */
    {{0x1a, 0x96, 0x9b, 0xc6, 0x48, 0xe6, 0xf1, 0x14},
     {0xbf, 0x5b, 0xa3, 0x8f, 0x85, 0xf1, 0x18, 0xb4},
     2, 0, 96,
     "8bPNOOQlQ+XeGQm7gXmiFV8VYwHewqvds6YbZ0uAg6+Zy6wX2yuWynxSVeIa4T1W"
     "8C8WCPoVvJu7R+Y/7qjhTIz10zb5OF2rcJpHDeKBQQbrSfmwKd0z7FClf3kpuCCY"},
    /* TrustAsia TLS ECC Root CA, sha256 c0076b9ef0531fb1 */
    {{0x13, 0x86, 0x61, 0x60, 0xce, 0x9d, 0x20, 0x33},
     {0x09, 0x69, 0x96, 0x01, 0x12, 0xdc, 0xc2, 0x85},
     2, 0, 96,
     "uH+lWz8BPn3wiG2uKZjhm1xTmdv3CP/VauCOy0Sm8MGMvU/UztSIU+heV9dOviw/"
     "8xKnAOmC4ypbMnxLmxQpnvgtg7XrjjFJPSZh6R1yi4m2CrucMzUP2uzeqUofyTOx"},
    /* TrustAsia TLS RSA Root CA, sha256 06c08d7dafd87697 */
    {{0xaa, 0x73, 0x27, 0x57, 0xc4, 0x50, 0x82, 0x9a},
     {0x94, 0xa2, 0x10, 0x79, 0x24, 0x0b, 0x44, 0x1b},
     3, 65537, 512,
     "wxa4G2qkRHPl1k70uc9bC8HRmoH1Y7CPQ8G7CFoaeuEHdiYfj2lWvv42YNAMg82U"
     "6ufFLVwvBRYCngqoL+VgJlSW+JhAHayunXRnL1T7lGOY4yZKg8aVtglDUM0Nfd5E"
     "DmAST1s1vb+ZOG19bNriaHMf+TGlGRBzBSrDMhmF6jSq3UIeMI0/nrUeYdVvvQBy"
     "cq0SP6qmSXPyhhWVDBBfUWTO/3e4yW2s5dWY8ZkubOPJRPm1QycITfZ+3kR5u7KM"
     "HNrTS2wu1sN4t0zVpOTa3IqODv8Pw6ZgJijPNr/6VwrsWz/pMBZ7xNvE1GSgMPvl"
     "/R1xkt0pj0FY3gCtOv09fBqosRfwTjR4WCXWhSHreR3Q3KvPYjywx5eL1tCf0/48"
     "3sXj/DqDcIQhHQnCofy7KCFlU2B6kG2WOFv/8dd6W23TyXBJSrodOtBQ2jIgGeSL"
     "P+vVFiY3PPt1nrAHcLgUYHfc9g9cUgq8XW2NUQraxRjIm23csIOzf6NOeEyYJavy"
     "fi4gXoIVptbYj6zlzTKGyPnk2oniOz6TxRNq/8f3GPxnte8YLFSrQNNsTe3FyLc4"
     "T2TElExPoM2upcYnJXUnbcbanI6wBZnVKBHFIdS4/MeWq0pkWWFrE/xLzJKmD6RR"
     "vw5YkNmCgzmmX7P6bpKzzTMOhajxKdOrWa3W297UK4E="},
    /* TUBITAK Kamu SM SSL Kok Sertifikasi - Surum 1, sha256 46edc3689046d53a */
    {{0x37, 0x23, 0x74, 0xab, 0x7a, 0x7e, 0xd1, 0xc6},
     {0xcc, 0x8b, 0x19, 0x8c, 0x4b, 0xa4, 0xaf, 0xc2},
     3, 65537, 256,
     "r3UwM6q7a9OZLBI3hNmNe5eA027n/5tQlT6QlVZC1xl8JoSNkvoBHToP4mQ4t4y8"
     "6Ij5iySrLqP1N+RAjhgleYN1Hzv/bKjFxlb4tO2KRKOrbEz8HdDc72i9z+SqzvBV"
     "96I01INrN3wcwv61A+xXzry0tcXtAA9TNypN9E8Mg/uGz8v+jE69h/mniyFXnHrf"
     "A2eJLJ2XYacQuFWQfw4tJzh03+f92k4S400VIgLI4OD8D62K18lUUMw7D8oWgITQ"
     "UVbDjlZ/iSIzL+aFCr2lqBs23tPcLG07xxO9WSMs5uWk99gL7eqQQESolbuT1dCA"
     "NLZGeA4fAJNG4e7p+exPFw=="},
    /* TunTrust Root CA, sha256 2e44102ab58cb854 */
    {{0x6b, 0x2d, 0xd8, 0x1d, 0xc3, 0xf9, 0x55, 0xb3},
     {0x79, 0x54, 0xfa, 0x94, 0x27, 0xae, 0x0d, 0x44},
     3, 65537, 512,
     "w83T/L0EU90MIDrViC4FS0H1g4J+91mfnp5j6HPa9gapTx+0+QsfOYyaINB+BtTs"
     "NNmGvHVbh4jw0tnUowqybBvrSSw+rF3YlAOg7DTlMMQ1ffsmTRtuMFTY9YBFnDmt"
     "nMklBE2akD5OQG6Ka80pZ8bMLeB06AVXCkhQ+npD2n7sW5oOYnb+6p0dhXLsEbs1"
     "6B8nv8Ghx7tIFt1W18xOoOG5rNvVgxkahdGUl9fKo2UL8zj5Aq7d9mfPyT/1iixH"
     "GplvBQ390B2CMfwpzABYl5FMgAAcM4WWL8tBwosQhMMJJIkftQ/Z2XdHGJKUYFzH"
     "mQM8/veVp31QoYDCqYOtWJZVIduGWdSvxrzdgW4H22Bi/uwQbtpoAfSDG6k+olsj"
     "12TG39yifdhLuoLSUfhmvwZG5HkqJjZ5jx9OmR2yjwwOHP/JXcD9kBCmsTfzzTok"
     "brSFkL+AuQyM1ZvWyPFWPxqAiXqp4hsyUSw+8t979l16KRmO5ci9NnGLXUzCHT+t"
     "WKLPPXBNplCYJdwj+bhYQQhxv0+4hKCPAFQV/JFtWKeWO+tLlifNa6KhhqwNfFTm"
     "ZkxmX5C+IZoCRi3kg8KAuc9LPuh/PAHsj17Nf9IoQgGViuKXPRAhffadHMU0oews"
     "DgpSLBJVcCQ9y8IUNUNdJ06+wL2qfJbn/J5hrUTTAJc="},
    /* TWCA CYBER Root CA, sha256 3f63bb2814be174e */
    {{0x7a, 0x87, 0xd6, 0x50, 0x38, 0xd9, 0xc5, 0xc3},
     {0x44, 0xa5, 0x4e, 0x0c, 0xf1, 0xef, 0x52, 0xe5},
     3, 65537, 512,
     "xvjKHtkJIH4dbE7Oj+NHM0Scx8lpqjpbeO5w0pL4BLNSUh1nciih34tdlQr+6s3t"
     "9ynO8G9/rM0977McRWr3KJDxYVfFDMSjUF3e1LXLGcqAuXXOKc7ShSLsAmPMRDAg"
     "2uqRW1bmHRzVnWbHP9+GyktTxNmNsh3q+NwnU6NH4WHMfbWw+O5zkcXOc2/O7hAf"
     "GgbP6SdgxU8Z5OvOIiZF12CZ3c5PN+B/52OtsLhZuNAGaDVg0zaucUME8WlleHzz"
     "H/PKKJ9aIJVmtM237o94pEUY6SYvjZspKLGktzptudQcOHJFWLFe6/Aom7eCyv3P"
     "1jMPn/uXnrEcnJ7qX17bqt1U6TAhKG2OefN1kowm/tzF9sOw30RZQ6O2Ayj2CDCq"
     "DTPh75ypByLjWVtAj9qIt2kIqLcjLkQJWTdbx+MX8iLrbjlSxd5Up5jJSyCV3EaJ"
     "X7QS+YUpjuvIJxUgwEvUzHwMbDQMJpsmMaY8p/bZ0EuiZP87mUFyweBwl/EkuyvE"
     "dCKxrGsiMiTTeCrAwKEv8VIFyT/vdmbiRdgNPa2VyMeJJsgPrqcDLvvBX/og4XCt"
     "sGUgNzNgsNWv1wwcwpBw10oYvH4BsLDrFR5EBs2kT+gM0cMgEOFUZZ62UdAadmtC"
     "Wlh2NOq3NxmuLnX5luXBWfeUVykljTpMq02aQdBfJgM="},
    /* TWCA Global Root CA, sha256 59769007f7685d0f */
    {{0x09, 0xef, 0x22, 0x90, 0x26, 0x22, 0xd9, 0xf6},
     {0x5a, 0xcf, 0x06, 0xe0, 0x6a, 0xfc, 0x16, 0x46},
     3, 65537, 512,
     "sAXbyOuMxG6KIe+OTZxxCh9ScO1tgpyXxddMTkVJy0BCtRI0bBnCdKQxX4UCl+xD"
     "MwpT0pyMjre4edsr1WryjmbE7isBB5LUs9AC31D2Va9mDsvgR2AvKzI5NVI6KIP4"
     "exbGGLhi1kclkc7wGRJNrWP10z91XynwoTAcKqCYphW97v0ZNvDikUOP+srWECdJ"
     "TO/dwfGFcJvK6qhaQ/xthm9z6TdFqfA2x8yIdR67bAb/m2s+F+xhqnF8xh2i90np"
     "FbU81qFh9RH3BW8d/RG+0DAHwimwCU4m3OOiqJFqH8KRRYhc5Zi4caUVGcl8dRHM"
     "cHRPLZsdkUT9Viig/ruGasj6XAtY3MZLdsirItlzD6X0WgKJP0+eIoLuonRTKj1T"
     "J2kdbI4yLGQAJmNhNk6jRrc/fbMtrG2QopWizs/agucHNBmW6bghqil+pji+jilK"
     "IWZ5H7PDtQln3tbUB0bzKtrmIjdgy4G2D6AP6ciVf79VkQV6zz0VwG/eCZQBg9c0"
     "G8xApfC4m2fVmJE7p4R4lSakWgj4K3S0AAQ837gUjujfqY1sZ5IzHcC30uySyL4J"
     "vywpBW8Ca57vvL8qvFvAUI9BcHGHsk23BKmEozKvru5rF4uysf5s4ZCMiKiXSM7I"
     "TcvzBs9fagpCsR4edy+OoOaSDgb8BSLSJuExUX0y3A8="},
    /* TWCA Root Certification Authority, sha256 bfd88fe1101c41ae */
    {{0xff, 0xc9, 0xa3, 0x39, 0x49, 0x68, 0x87, 0x2d},
     {0x37, 0xdd, 0x10, 0x78, 0xee, 0xf7, 0xfb, 0x96},
     3, 65537, 256,
     "sH5yuKQDlOan3gk4kUoRQIenfFlkFHu1ERDd/r/VwLtW4oUl9DVyD/hT0EHhRAHC"
     "tBzDMUIWR4UzInayCm8P5SVQT4WGvr+YLhBnHr4RBYYFkMRZ0Hx4ELCAXLfhxyt1"
     "y3yfrrXRnSM3Y6fcQqItkgQbUMF7uD4byVYEiy9Sm62pVunB/62pWIcwtoH3l0X8"
     "GVc7K2/kR/SZRf4d8fiXo4gdNxxcj+B2JZpQ+KBU/0SQdiPSMsbDqwa//Pu/8619"
     "kmICWynTNaOTmkNkYF2y+jL/OwSvTUBq+cfj7yP9a8vlD4s4De4K/P4PmJ8wMd1s"
     "UmX5i4G+IuEcWAO6kRuJBw=="},
    /* UCA Extended Validation Root, sha256 d43af9b35473755c */
    {{0x9d, 0x73, 0x63, 0xa4, 0x1c, 0x7f, 0x6d, 0x2c},
     {0xc7, 0x05, 0xc6, 0x70, 0xfd, 0x8d, 0x0d, 0x76},
     3, 65537, 512,
     "qQkHKBMCsJngZKoeQxZ6c7GRoHU+qPrjOAB67IlqIA+LxbCbMwNahsZYhtXBhbtP"
     "xpxATcq+7mmWuK2BMJp8kgXrBSuaSNC4dj6WyCC70rDxj9isRUb/qmdgtHd+ah88"
     "GlJ6BD0HPIUNhNAfdgr3ahTfcuM0fFdOVgE+efGqKTts+viPbU3INd+u69wk7nlF"
     "p4W2BYjeiF0lfJdkZwnZv1oVBYbzCR7sWDIzEfN3ZLB2H+QQNRcb8g6xbKQqo3P8"
     "CR8eMhlTEefZsywudi6ho95+aogJ6PIHiviyzRDn4nNAk7sI0T/h/AuUsyXvfKbX"
     "0a+f/5aa9ZF7mAt31H7oB9JitZU54/PxbQ8OZYSKY1TFgLbgnkt9RyanAQhd0Yie"
     "18MyRPqCSgpoVH84UwPMpAAzZFFZC6OCkXpe7BbC8yrmYtoq21liECVKKoELRwdD"
     "BnCH0vqTESl6SE3rlMdwTa9n1VGxgCABAbR6CKaQf07g7wdBh69qpV6L+89QsppU"
     "r8OJulgt9TCYsTZyOX5JBP0pp0x55AVX25S5FlONRrMdlWFXVn+v8BZbYVhvNlAR"
     "C9isK5UWGg4fCM02NGUQYmbVgF8UIF8tDKB4CmjWLNfpbyvSSgWT/J5va2f/iPFO"
     "pWlKUjcF6sYWjdLEmdGCKzu6NXX3UVFY88gH3eS0A38="},
    /* UCA Global G2 Root, sha256 9bea11c976fe0147 */
    {{0x47, 0xd2, 0x91, 0xc6, 0xf3, 0x9a, 0xc2, 0xa3},
     {0x99, 0x01, 0x13, 0xba, 0x88, 0x68, 0xae, 0x19},
     3, 65537, 512,
     "xeYrb3zvJgUno4Ek2m/LAfmZmqkywiKHYUGRO8vDaBsGxUypK8FnFyIdK+35KYmT"
     "oni9kmugow2ifsqTs6bRjDXVdfkX9s9FxeV67HeToI8jrg4aA3++1NDtLnurRiNb"
     "/yzmVHqUwCoV8MmNsHo7JOHXaOIxPAYzRrZUEaalLyJUKlgNAQLx+hVRZ2zA+te2"
     "G3/RVogvGjqNO7uCEeBHANBSh6v7hn4PJGtAnTRnvI3HLYZveT6OqTwXS3+wmeOw"
     "cWDcC/Vkw85DvG1xudLeJ1uK6NjGruFZfc8oLTW4lVYa8bJYS7cSN8h8s+1LgOGN"
     "+jIjtm+3SJUIsUROhYw6AlQgL9+/V087OpAh18EmNVQg7Mc/R+zvWr9LesGtOxdQ"
     "XGLYD0tK3Cv6brxzks3sx1DoQZbXqX5t2Okdj4q1uViSukqSKwxW/YDrCPBeKW4b"
     "HAyvj5OJrdu9o54hyokZ7N+1wxrrFv54NkzWbtA+FxyQF2smuvt6L78RHBgOLXMD"
     "j6DlNaBa4kx1HXHhOThTeEDMg5PXCp6dW4+K5OXgSORIskfNTip1KnvyIvbJvgmR"
     "lld6iIis7nCs+dwp4wwcOxJORNanTrAmyPPZGpeRaOrvjUYG0lZFWJo8DA+DuAUl"
     "wznPO6Q0ibd5Ei9Hxeepl2n8pndntd978XplFeRhVmU="},
    /* USERTrust ECC Certification Authority, sha256 4ff460d54b9c86da */
    {{0x56, 0xbd, 0xb6, 0x6f, 0x42, 0x2b, 0xf3, 0x47},
     {0x13, 0x20, 0x6e, 0x11, 0x80, 0xb9, 0xa3, 0x2c},
     2, 0, 96,
     "GqxUWqn5aCPnetUkb1PGWthLq8bVttHmc3Gu3ZzWDGH926CJA7gFFOxXzu5dP+Ih"
     "s8731Ip54KODfi2X0GHE8ZncJZFjq38wo7Rw4sehM5zzvy5cU7Ffs30yf4o043l5"},
    /* USERTrust RSA Certification Authority, sha256 e793c9b02fd8aa13 */
    {{0xe6, 0x59, 0x01, 0xf5, 0xfc, 0xfc, 0x65, 0xad},
     {0x08, 0x1d, 0x9d, 0xea, 0x67, 0x92, 0xd5, 0x96},
     3, 65537, 512,
     "gBJlFzYOw9sIs9CsVw127c0n00ytUINh4qogTQktZAnczomfzD2p7PbPwdzx07HW"
     "ezcoEStH2jnGvDoZtF+mvX2do2NCtnbyqTsrkfjib9DsFiCQCT7i6HTJGLSR1GJk"
     "23+jBvGIGGqQIjy8/hPwhxR79uQfjtTkUcYRZ0YIUcuGFFQ/vDP+fmyc/xadGL1R"
     "jjWmp2bIcmfbIWax1Jt4A8BQOujM8Ny8nkz+rwWWNR9XWrf/zvk9tyy29lTdyOcS"
     "Ok2uTIq3XJq0tyA9yn8iNK5+O2hmAUTnAU5GU5szYPeUvlM3kHND8zLDU+/bqv50"
     "TmnHa4xgk97Exwzf4TKuzJM7UXiVZ4vuPVb+DNBpDxsP8yUmazNt925H+nND5X4O"
     "pWaxKXwyhGNVicQNwZNUMBkTrNN9N6frXTpsNVzbQdcS2qlJC9/YgIoJk2KOtWbP"
     "JYjNhLixP6Q5D9kCnusSTJV882sFqV4Wg8y4Z+LoE53MW4LTTLPtW//e5XOsIzst"
     "AL81VXQJSdhJWBp/kjbmUZIO8yZ9HE0XvMnsQybQv0FfQKlERPSZ51eHnlAfV1So"
     "Pv10Yy+xUGUJ5lhCLkMaTLTwJUdZ+gQek9QmRkpQgbLevni3/GcV4clXhB4PY9bp"
     "YrrWX1Uu6lzGKAgEJTm4Diup8kyXHAc/DVL17e8vgg8="},
    /* vTrus ECC Root CA, sha256 30fbba2c32238e2a */
    {{0x12, 0x61, 0x7e, 0x97, 0xa7, 0x4d, 0x0c, 0xd4},
     {0xc5, 0xd2, 0xb8, 0x48, 0xba, 0x16, 0xd4, 0x78},
     2, 0, 96,
     "ZVBKrox5lkqqHAjDo6LN/llWQXf9JpRCux3NCNtzslt188+cToL0v/hhJoVs1oVb"
     "cnDS/dtitN9Ti72xRFhiQgnH+n9bEOf+QP3A2MMrMudwpremIFUde4BdS49nTPEQ"},
    /* vTrus Root CA, sha256 8a71de6559336f42 */
    {{0xbd, 0x8e, 0xa5, 0x4d, 0xe3, 0xec, 0x9e, 0xbe},
     {0xb7, 0xa1, 0xff, 0x1d, 0x55, 0xd0, 0xf4, 0xfb},
     3, 65537, 512,
     "vVV8YdO4HQRiBaCubLdwtEHqSwNeED+QWhyLO7Bmi2xIphwiutVAku4zsiNZyY68"
     "WNqLntAZ8i9ZxoxjWrqfowuws5pcuhG4EukMu89ubICHKRQDLI0kmshkg7VqrBMs"
     "M/Gf3CxhPBo/cFWbrQBSf88Euf42+pzAFq5i/pZMQ35VFL4as9Jtwq92ZpVrKrCU"
     "d4VeBA9iHWN192vny1uacOw+ZwXw/gcIgM8o2wXGFCcvhn3wJ97/5n4zSOcLHljR"
     "JytTDldKZdf7ooBg/Ey8NVMBapdygq/xHXDonPXvXsJsx0d+WpSFJk07uutM6LAJ"
     "wmXCnZ0Jm061lwWs9Qag9zYFfvSQsmvEtPlk6ukaCsgNqO0nydTns7mrgiKQJz0q"
     "6HyQ77xP/eIKJKfeZSSkXerAdjDTd1D4DQSblDYBc8oGWKbTO9z6BEYTVYrJREe4"
     "UTkaLug04nnLWUoKf7ym7x8DZ2pZKyVik9lTGWY8J2Iphk2ka+7/1E661bTijkha"
     "ABkJ8QXZzpGx9+vpOU/2bwRDmlX1PgUUvb+zWbTYjjOEo5BSqrMClWD5DExo+e7V"
     "Fw34cVe1JeQp7mVdr9HuPBcLWkPFpYbqJJ7iBQfcNEISkdY5dK5MQYLb8qZI0bOb"
     "8zOq86bAxU719J12Y+YCxiJLwZU/UGQsVOW28Dwpz1c="},
