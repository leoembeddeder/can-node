/* SPDX-License-Identifier: MIT */
/*
 * SHA-256 (lib/sha256) against digests that do not come from it:
 *
 *   - the three SHA-256 examples NIST publishes for FIPS 180-4 (one block "abc", two blocks
 *     "abcdbcde...nopq", one million times "a"; they were Appendix B of FIPS 180-2);
 *   - a table of the digests of messages of every length from 0 to 320 bytes (five blocks),
 *     computed with Python's hashlib by tests/python/test_sha256_table.py, which also checks on
 *     every test run that the table below is still exactly what hashlib computes;
 *
 * and then the properties of the streaming interface: every split of a message into two
 * updates and a byte-by-byte update give the digest of the whole; the padding at the block
 * boundaries; a context that was used before, or held anything, starts afresh; the length
 * counter has 64 bits; refused calls change nothing.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sha256/sha256.h"
#include "unity.h"

/* Messages of length 0 .. TABLE_LENGTHS - 1. */
#define TABLE_LENGTHS 321U

/* The digest of message(n), the first n bytes of message_byte(0), message_byte(1), ...; made
 * with hashlib (see the file comment). */
static const char *const s_digests[TABLE_LENGTHS] = {
    /* BEGIN DIGEST TABLE (tests/python/test_sha256_table.py) */
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", /* 0 */
    "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d", /* 1 */
    "58fe7f64eb18129d249a9079a86ac69522137605d4ba96d8713fd2d8a0affc9e", /* 2 */
    "c3623df2fb150e8acc6f7c8031751588a1f79ca0b6bfb35af1f9f46d04242593", /* 3 */
    "bc2df815a51b2bfdda54ab3146779135ee4150a1c5fe6095f85029300c75ace0", /* 4 */
    "16b1bff42ae6a3b4cf47df4bb14a2659d0ff87c341821c539d89eacfcb3c21d0", /* 5 */
    "69211ecadf24589271ecdf85dc7fe7dda678d8c94d28bbef3f35144dc49bd941", /* 6 */
    "84befd231278e2e86d0ba779379254ee107481e9ec8a39288afbff62e6ecf93c", /* 7 */
    "fc3d635f7bcd89d4258c699355227c57abb24f2b781cba9641030abb1d25b97b", /* 8 */
    "a1511d5a16632fad1b0c8f5c72c0e46fe0c3808caf4289bdae7dc6355d1a85ba", /* 9 */
    "56ec9eae762e25e71bae6324c2843868c4f9b69d22a63904767772591810fe76", /* 10 */
    "84ca26fe76ba725bef9aa3a5fcf2cce2d9f7353aca0fc11fb4e78f6095aee607", /* 11 */
    "1c5239a55d378c93f727beee5a5ae494ec0b16b0e6af73d4f7172d7f5c691cee", /* 12 */
    "b36482b38a98974f63ff4faadbb5e9d514e76df2a4e72872f1ba669afb20a18a", /* 13 */
    "bcd63c416207ca596acc533d3584d35b188dbd2cfc24fbfd53b4d51d204534e3", /* 14 */
    "12f16dc93a58cae9e6916b8bda2493698fe0426cf4f7b02eea54bdff821e5253", /* 15 */
    "66f7ae01b78f73b461abc2501dc7e4db8be25cc50b37d64b6b44cc829613c51a", /* 16 */
    "cfbb3583a873cb686ee3ae8d6f0d3faf497bdfe7cca3ffb09bb7952226e0da53", /* 17 */
    "ed9923bbb7502b4485a7c2af285c445697a562993d6029be1f77bd61a51a46f8", /* 18 */
    "33bb3ef40eeafeb946d8f8491c7d6b43f2e809613ae0b9b0d567eacb990a38d9", /* 19 */
    "0fab448aa82dc3c3ef3a7f715e566568a47186a6be950a3108649bc3144b20a5", /* 20 */
    "9311afa55898834864e48f8bd56cbbc96e617e72c58e594ab7eaacfc3ea71d76", /* 21 */
    "a312b3d82eccfe0cac4d91c0a87651c6546d4207bf30d14864372e0beb6f9043", /* 22 */
    "53c9c01075cb804251a72ad5ef8d80a619e442fe7398545e7e05565c1bfe744a", /* 23 */
    "4eeaf302031ee8ba4ed1cb3534f902cdea0725dd9d8003d7ae8a885a78245f72", /* 24 */
    "98cde22da434b151526ea7699a479d1df73ec284a832bb7c1239403eec93f942", /* 25 */
    "33b803f10d5bf486870bff67b3d6e63a7f9cf6c912c466b51580a39faf800dfe", /* 26 */
    "5355b653a9f4b2b57274ce6bb600aaf050e7d56a21f103a8f2835f36dab31d61", /* 27 */
    "ecf302154ec24f471d1b76d40d2a2cd1bfeaf966c811919afad7df989167a41f", /* 28 */
    "d5e8303b37efcf59aa24b72070665523bb3129fa074f04ed02f3bed8f6f5760c", /* 29 */
    "876c673010e021e6e9650de4117f5686e4e93282981f4f459da690e8bba02d24", /* 30 */
    "776a2eb8fced6af728a9ca1d97f5b579df55204c56c991f1a1c04ac90f08ed35", /* 31 */
    "66c1f393b33d5789d4cb94f3f2eab4a8449e403456eb0d50b47f1a2cb05b9472", /* 32 */
    "d0d1a975b2a126302e1c3009bd40a6851ca95b2fa0a6f7d0464a07b1eba36b69", /* 33 */
    "9f75cdaa3e9ec55ee44af854b42f07a4b4ed953eca577adc2a87c262ddde32f1", /* 34 */
    "3be3d46f57eb810b5a5546c7b86cfa20b5c5105a3048aecd25ae95d694063cfa", /* 35 */
    "52b137975e5944f5ce6dacc003122e96413369967a799401b86849767a0ad9fd", /* 36 */
    "ad9a067b86503257c9a588fef2f1d5227852058cfd5c5362873a0cfd399fb1b2", /* 37 */
    "e44b668c6ad9f52d7917080dc52d23479ea23dd83507e526343b379792b1ff43", /* 38 */
    "f8d5db06abc8771d57e828072fe2a6824ca13d07f323aa2412933ca78169f326", /* 39 */
    "b25c3ba44ccc0d216e0593ca84122c9fcaa74e2322518829b6b1c2f2d4c72602", /* 40 */
    "8b81fe688bb602b1953aad32971c3715fc1ccd4d5e7333f1b44698fb58893b41", /* 41 */
    "815280c7367976127d289b0c1481dac3eee6feba56335c4182585ac0de6c8d43", /* 42 */
    "84b5d39c2eb8531a145deb06fc8b679b99301941263dd911d9f9eb3722173487", /* 43 */
    "f91ad168999da24b3859eda6db204b6f3de27a06616c3a2160b7bcd7ca1588db", /* 44 */
    "a51d4fff61b10a4ba1436107e804c79878da9b2a20a017cdad79046a4ceb5d4e", /* 45 */
    "9b848e5e4141db711f2bb0531081efef7b6b86ab4773a2a9d56683ac3326a147", /* 46 */
    "29104724eb252479686d5ec6f2586a39fea1c0ce92b6bf30a59c33b33735ca35", /* 47 */
    "8961729a02502fc14023755037e6486bfd673c6a4aef61eaf9aa47fd55b661c4", /* 48 */
    "3c60adb87d5b80305f6318615a64848d175133cf09eb217c01718b78bd844e80", /* 49 */
    "bd9b54cea6b27efcf0701d49667b6dd210f0a6e176cf38c23bf1d21b365dcb8f", /* 50 */
    "f74b744b9b080ba3233a7d6d4f6eafceae0f5cd7ec1c745ce50fca74d6e415f3", /* 51 */
    "026fdb834772d9222f27e666b0da3931eabd4f6297770f4b6cf022c7668460aa", /* 52 */
    "af43025d4925a867ea05e735157247bf1afe7f3ffb655cb2faf88c5f9c40cdd1", /* 53 */
    "23978a03db672e017a955bb3a575f386c4ef923dda2ae48cc3bb145a42af9805", /* 54 */
    "87a26dae81d37e91b25fae7899274e680a048ea44f82b18397b6585c2615c6b7", /* 55 */
    "cd6756cdcd1cf71057f210633c6d13fce8ac4b9f97690b42c0aa28d721e2add5", /* 56 */
    "b094b6fb0f9735123d38ccb6df5796f2ddc2ed4035a7be46fc669d1af4c72f76", /* 57 */
    "094b177b1bef94c6ba691138d691cb252745e1a04e288ebd8025864b4eb8d21e", /* 58 */
    "bf3d82c91ae441142d70c65c2c6c8f5f656673b0afbfccfc2e376dabce124ce6", /* 59 */
    "b6e1171e0ca24cabe4e43aae200d1f7158a38c2f0b08e307c426a2647502301c", /* 60 */
    "74d6f362f819ede8bf4c6c1002fbf6bfe1591994cd807e0401605445fc8e7773", /* 61 */
    "9c48b293e1b8d559a36198d74ffb40ce9e071e4717388be35bb20a2308fb2d0c", /* 62 */
    "7be910d529434dd8a31078d25667457b6040c888d64904b25d3ca95f1eae6b16", /* 63 */
    "51e945469a3948debf6fc154e954fd6623ebbc21da2a3ee5e3ee0264e2cef6f2", /* 64 */
    "3c3e113ec16f54924b2c563c7304d9e10ed4d9b54f266fecfbdc63978a14c9b4", /* 65 */
    "a8d19bb0fdb8fe06e357761261f9033e4d2cae9b62a0bf4c7148dfbe7575de5d", /* 66 */
    "1948b28e4727dd6b7c8d61179f8891839ff8f12cc62bae95100ed1d7d8538dd8", /* 67 */
    "e905b3bd2fc066d802b542e82593c6a41836a8af1249f12bac1d103ae62a3dec", /* 68 */
    "a6622f3722834102334a41ffb8fb9223da77048e7d8260a1069dd1dcf7afe98b", /* 69 */
    "6b6f0918edfceb999a450fa8cdbff4007b0d668d49ba40dfd9bb7ef1a35d62c0", /* 70 */
    "caf659a37aafed62baeb9747bc9009a70589a32c64e271d67fe70116ed797160", /* 71 */
    "560821f9622b272906ff9bb837b76f541ea63424dbbdcf647fd7b9d451fc7704", /* 72 */
    "9db719460336bcb889ed17efeed074caa5d294d4848562357c7e5ff42fc3393f", /* 73 */
    "a4e2b31c578ed039006561dc7b67854d1dcf74b4adf2646ec1515e66d71319fe", /* 74 */
    "d1963fb3d15426aea5a0f831309af0c6b2743e53bdea9090128ef05c43e46bd6", /* 75 */
    "5b15f443d5292ee3de9aae641bac6f7909de110305945d6f188f4312ae11fd76", /* 76 */
    "896324cce36cf641237722b7d3babcb7f79f24ef721a727a9de9dc069f5b2cbc", /* 77 */
    "61ba966c7efefbadde429e743dc114b13d43cb3d829f0caea49d3304e96e1b88", /* 78 */
    "23c47e9915420c95775d6a182a34553fb7cf359aca268b50b58b5cb523ec397a", /* 79 */
    "fa7c9d12b8a09a30744f0f83a58d73f890b94837a1073f1f9ac5ce2be89f54b2", /* 80 */
    "f467ba10cec33bfaad814d88185c928daff065c12b5a137c3a904dab9640a7e7", /* 81 */
    "41c92f93964778babd53d5beccce2d6a874957c5d7c0f490c9064137fb1228b7", /* 82 */
    "af17e72d8a8e340562bf9e8e96619a45e02963cd277560a88ecea26201ccb12b", /* 83 */
    "be2fbcdf4053b0c66ab345c2412490f63f663b5268079660a394a3d737c3cc52", /* 84 */
    "67cca79faff94c4fc21b0f6d8b67fcae9536320dfb7c7b004f25786e98497a8a", /* 85 */
    "cb0c3d44703d01f17a7f8c389d7d890570a7e4de4e7973f03cc6c6bbf7b1f926", /* 86 */
    "26ca1994748c72d8c6b3bad01e944ea37f5b22c9b669a3fa0f001a180d606104", /* 87 */
    "9a30c46ef0cf6c2ac91deb21dae948278ba40418fc0e376bfd12f892f07a5790", /* 88 */
    "d14ec6f138d6947e38b78b86fbd81da76e7758a2d7bf2fca25a7b03000009a5d", /* 89 */
    "4f490b7a2a024f8f5e0ec82f3a82f9705b025c1063ed2909da986605852db583", /* 90 */
    "58a87126928dc3517d61937ba163f800770c6da8e2f33a72750db547af15d710", /* 91 */
    "f3ab7fe7031f4435c0e461e519d703a187a9c788762fe1cb7312ea76883f07bb", /* 92 */
    "b6ab5ba2aa08c1f2321c334fb96c6f92b9428e7d117dc56f719cf651fa027880", /* 93 */
    "663dc4be468f8dcdf1d30d03aa696b6f86eb9276cccc88e016ce839113df31ad", /* 94 */
    "eb7f5f8760720596a620b90491539932d2b5c2999655587fdbc21dcb77832f86", /* 95 */
    "d95986af387386d6246213117e12865016f19252371ae3ccd0edea6fe1416673", /* 96 */
    "b3b3cdffe86b7f10079cae3043297c30f631663b01d7f57207aeb14192a85429", /* 97 */
    "cbf9a679875955d193dcf0e636d5c5a9a35905ce5498d9ca196c9b6ea522587f", /* 98 */
    "fb04c8106fdd583afa487a4fe9bca1aea02914e87c697ed0bbd4210c66abb541", /* 99 */
    "76f01f23bbe35316f9a543ca06c1ab3029823195d04bbd30f80e3abfbd8428e1", /* 100 */
    "a2950692f126a98d3d5ca6d800694a26751f419397b5e2f30907a104d0a71f28", /* 101 */
    "8ae1c46bafe19c378b225c300f4c7c001f9b7d8c7b3d076b1c8766a7c033ca8d", /* 102 */
    "9ff356ad9390fc7b76487d3bcb8b374dec793de505aa508a5569efe8a743bba5", /* 103 */
    "f1169505687f82b05e4b8e4bd4065c22afbbeb6c23cdc58fea7e6d683bb0e7cc", /* 104 */
    "bbfd09508f09295803919f8ea6ee6cb511a2ae09adb24f492fbde5ab4f6210ca", /* 105 */
    "8c1d6386baf819f0c65d426bfe39a1602b05a4e67da210a859c24b2cc62c805f", /* 106 */
    "2a1730670549ae648e7e5cc08e16085a4078d57faf4c061f5c348c8625731fd2", /* 107 */
    "b6402f83e99016964b77496ab1ba67550a7e5aaddb610b27500265df8e7f2f9e", /* 108 */
    "4cacf27912214d92f8d396e29fe1ee3aa7f4d7f096b54e33dd8d3f4ef1056624", /* 109 */
    "666e7aee6879790d80952f6f124af021dc62536a2bfb327fc0131a6942eb49a7", /* 110 */
    "d5feceebfb0fa7ff625f63035c8ff7f6b80cbb5dbbd44c628a78f3ca30b939d2", /* 111 */
    "3ee6a10b7bceaf0ac781e1c84ef0a7d8a79f85ae4e848058445ea0febf9c3de1", /* 112 */
    "97bd33d3d0387f9af53057dd1ed0d9b94b559f42210a52568a060c8da02cc74a", /* 113 */
    "6a1a9cc9e8d3f000fa6b33b6286acbfd7e0056e555bf2f2dc7063893bdb082ae", /* 114 */
    "8d7459b692b620b09a9241dccd7c1c737d52e303868b0cf430a2e4dde1686d57", /* 115 */
    "8f2f7fa4d7e66b90e7ad78aa7f7dedaf350a7bc19c405bd4c221b59efa8a4363", /* 116 */
    "b5bdd5cc3900054982db3a0b386abd0a4d9e58c76c2a8c27f3ffd922313b2280", /* 117 */
    "4c6a8866800385148643220d6146027b9ecd486c8516a369b6462e475af98269", /* 118 */
    "c9128add91b549276c05fa0088f24600737e4a1eedf505afcc5977acbce20d17", /* 119 */
    "05bb0826068da152ddc644acf1ebc99edf92eaf9bc41709d94d1f0a8a456764a", /* 120 */
    "cfbd04db30a73b8fe2541ca98c15da2164ab5caf498cd535d572259d0c6c694a", /* 121 */
    "eacfed1c5a5e36cef5b2cf72ce877042380ab98f37673502a7ec407c78798ed5", /* 122 */
    "2042e7d543ffb0ff7e0de7890b49e8a2447584180b62fb324caec3d8846035c0", /* 123 */
    "a600e1aeda484c79391dfcfd169e2748ea652798b889680b4651c5f1118714eb", /* 124 */
    "dc6a503140253297c2a0a14e4439d21ceefe183c7f0ba711906928a455b01e96", /* 125 */
    "0ea40b59e63371ca66fae30d6207be1f6971fd0a28797808a20baef105610d23", /* 126 */
    "17999376576db9d8424b7e43f9d2d36e43431be6900f01c6a4e6137959846653", /* 127 */
    "775ad69e0e164f428bec38443d0b19b3cef04a5304446e1a5e804e256c93a66a", /* 128 */
    "99a0f1e6a2dedc5da9666076efc191eaeed2a4845bff9ee1ba02398d711a6653", /* 129 */
    "a212e494b81c9178f5b731388ced50b0e70f0b74ad037fa4b9e75632d4614886", /* 130 */
    "ae924d022d80990eee6509b31418bbb4965e08e8ca2ece349d90d3851f34b892", /* 131 */
    "b0e1693add4d14618ae28398033119d09923104807e754c05689c1b1a4fd5624", /* 132 */
    "740affa51f58ef4d57f8e03ef31d836dba201748d22299513efa34f2290926b1", /* 133 */
    "c40aaf92b5a5d5e0f5a6a93d88c02f83688566701fdeae3c47bef8995c15590f", /* 134 */
    "e58614cc503bef13780c64c3fdb00d7d049dc724385529f8d31b9aa78e5e07ff", /* 135 */
    "499cc22e366eeb404f7319564fcb789aecca20f842b409582cbbd31f4b49c02c", /* 136 */
    "720bd800aad57e598a6d5ef842a99576c7e07583ab18fdbbb72cad26fba939e2", /* 137 */
    "d2e3dcdadb834e2bcee9d883c540f53f473faff80a556b397f27accabb0b4bd2", /* 138 */
    "3081a259b31c8af928859a429301bf89b73a424cbfe0bf9a30d2dc66224190dd", /* 139 */
    "f682b65e28aad9150d7ff912c8a3116cad29ce0b359297f78f91da10ef8a11a1", /* 140 */
    "6b5b11dd6f0142c97edcb61c9eb6d744d88f106d297ac9d64332db4b9b010d24", /* 141 */
    "878ee417d7f3b8ee470e910ceb870c71f282a4a6edfd92b0471a384dcea34b2a", /* 142 */
    "690bafeb2afa33810c0ba80105da639cdc436aeab5704ea39df2e98a4c8a9c48", /* 143 */
    "c24ad1de13fd90111263a550a0d3a966a9a6143a385e5610cbb1e9c179cd77c0", /* 144 */
    "6c1541405a4aac8ce630070aa951b24b63c2174534a735b226b166eeb121c6ee", /* 145 */
    "caf482a3957b37429c987c1780f554e39131833ec591d31caec4e8ef40068614", /* 146 */
    "b1b5133df10f43a0d9423c66c4c3141c2f9572f100c2f5e0c3e568a99744b56f", /* 147 */
    "a5e6623194395887f932459eb3b9102320399300b8af9b36be0a7e17d0a5ed75", /* 148 */
    "cd371ef19d65a6b9cda762f477c6cb2ddf981104329f2a7f1e966b74ae22685f", /* 149 */
    "581b2308a3d2549341fe5af29be5d5e038762a6bd2136dd4a85c1e1149e89c2a", /* 150 */
    "c1d9b25bb5489c99ae1df429472b1304f7c1bfca97b114b4a96c1de7d3bf2ed1", /* 151 */
    "b76b42125512e04a42a71fbd3788f9144d14198c0eacae3c9423a99d62b36cf9", /* 152 */
    "982e96c29123895627b5658a1d6217edc8097cb5ac644d19deb759bc2c259edc", /* 153 */
    "8bdf554dafb556969ad673774a8b8993a9ad2fe00f1c15d5b54bfe7b49c334a3", /* 154 */
    "df34ff607085b6566133c2a882272cf603d9a4677309d57c3a1721401e362aa9", /* 155 */
    "338d34dfc8ae16ce48410beac474504970985e466621d9ac1e34b8772db0f93b", /* 156 */
    "843158f503517b667d465604687bdfcd91c151cd2f4c6b9abc9de021080dce71", /* 157 */
    "cada62ee56b977bff4a0167ab41eb2c15efd7ecd3796732aeea16929e7d74091", /* 158 */
    "29f00a295f6e207bcf18c483c61030b50f8597f3e70f7aad7662143405c23aaf", /* 159 */
    "04c1372a5eae47808c2088ec2e87fde0fa13e728362e6b1d8132c9a21da07fe3", /* 160 */
    "d69d8c63d5b0982ab4a6d00813bc78f4e47e03e3b1e3740f5547f79ca44daefe", /* 161 */
    "fdfc2fbd0b752aa07d87cc9e5dda8774d1a3281ded00b7d4f2c21e46be72137a", /* 162 */
    "ad351ea0f1aa73f0c56723f3bc083850d3589e3937609eeaf39a19b39813e8e2", /* 163 */
    "db306498579b54e969b6714cf5cbe7b876819f7e8156b5f23c4401c409b8906b", /* 164 */
    "5e8d1a312c969de3f5868c6c40e338382b165fb80989e63a5ca7541d60bdbfe8", /* 165 */
    "67ce0f2b2d6524018e955c09566550a45b75c09d36cc6455baf311c825cdba19", /* 166 */
    "ec9aa5900f195afcb7cc6322e56d165326c61c19f084570d1f4b8ebb6ddcb3f1", /* 167 */
    "bf9a00e38917f800de611c1ead6050a2fd61bc36a38cbac2f18cd3eaa6e44607", /* 168 */
    "dac9334fe4354d7b1553157425b9ed80894dbd2d0d279a7188bccc5b16e2e81f", /* 169 */
    "669ab0a28a2a6862c5bec50bccedefe29dbeefe93d1f13502e9728e6322f9dcf", /* 170 */
    "ed3ea97fd5e502f737784bd0dd7a134e97ae070ec4c4908c3c25f70bae49730d", /* 171 */
    "fbdec6f40e70247bcdd2eb26a64422abf98548469337460920cba386020bcbed", /* 172 */
    "b91e1370bd7f10b63d2bb28eebb1e46aa4cea5cfd416398d7f49e694df3e6d40", /* 173 */
    "dc1b2d737d44b447f33bf830eab9ad6fa1ff92801682789aeb112bdbc4ffdfad", /* 174 */
    "2518a5fa6fc79fc8dbedc26c824fd1b4cbcd45ef133a1b99428aef8cdb1cd13e", /* 175 */
    "e4fb2015b47079d98783a1bca8a2465b398c244b4e96c922a776d809baf26661", /* 176 */
    "beadc06ad44d89c84f3762fa3de3124ead5d284cd72e0e03c9f1ef048712cc21", /* 177 */
    "d227034cbd1137b5d2a920cad9003440b0589f0115a93e3672b92f4b61c3b4ec", /* 178 */
    "f743d809ee2e158c3ee503b6d410cde7603034fcdd8563e6727ae848a15c998a", /* 179 */
    "bcfe31d61a85d07c85ff277f5e0e53db4e3c4698998f254b05da170c8c458514", /* 180 */
    "0a2d8d31ff854ba3d03ce57699003a66705334052e3dfd6fa19fa8cf69bb1469", /* 181 */
    "e657640d3a10a1bc3cbdfec08b0eb794149a0bce71e6f588d86eaa73ac56124f", /* 182 */
    "677692b41b915b1b8b7bd29115602dac5c1287761ae4252635f63a9022dd7d7a", /* 183 */
    "8a8e43127317c175be3fbdc0046d676fbd000d46fa2d14da9e0c174a74b92d65", /* 184 */
    "d2f9c6bdfbdfcff70e18e5fde952dbfa087dc5121ede72c62fb9a21dac46b566", /* 185 */
    "b8cb85dc5540743ab31e5c095bb7812f167c9cbe3ef2dccfb68727c0dbcef064", /* 186 */
    "3b62319c2d20cd6694cf92f98f830e79f3d225f4d595b28d2f18e7330eb89ac0", /* 187 */
    "ccf29c8e901efc84d11dec97be87f582fd3e18520c189c07341891ce3f87f9c2", /* 188 */
    "9247f4180ce7304fabab6be44e4f2b70b64e0a21c893494136f0f9b52797aa43", /* 189 */
    "1a938f13db882d0d29270803495093ff1d8fa7ba483605e747431e134d8422ea", /* 190 */
    "ecc939b10ca9350313faab18a730731550a5529e31794efaaeaed33172e5de28", /* 191 */
    "84aae10bd92aa2e41f54a82199e71ecd834489536566cc63ee5b69ba0996ab78", /* 192 */
    "3604dd8d18392bda6c3fece257a32ac563a0dfc4d389ad2d21bbf48ea6b28a1e", /* 193 */
    "daf936284bfd0448d2786d0c8971e371572e8beb630cd0534938c7546dd29cdb", /* 194 */
    "944cff75f764a309aa538cce16ae9243659091585adc05ca068cff754653f584", /* 195 */
    "c098c24d997e4d62240805fac423c8468f3b6ba2e9efa80021cb5ec916177dc6", /* 196 */
    "3af9845d4fa4bd4fba8dad98c3e730f1b9fe43201abffa357a5a9795de13b6cb", /* 197 */
    "bf88b0440bef64f928a94b0b2cb902df1ec46b3cf9211de7d6305ce416ea8599", /* 198 */
    "ddeca13a9773e4f0caf5f9ca0261ba2aa7ef19240d95801cfa7cdda2d2b588a5", /* 199 */
    "6216658ed8f0943a6eadae71afdf8123415939b9795f1b79e778fec09b1397bc", /* 200 */
    "2fad31c93ddf50385a6e0874cd266c54662343c30a0b171c4111f3894fa0633e", /* 201 */
    "818fb7dd601181ad6269bac8b1e9c0747512357d153da808738479c1b8a0217f", /* 202 */
    "89b468ecd71f7dd0c58716cfe08f47095149bbd642f2eee6c80fdd0b25d9d3ec", /* 203 */
    "0354c150abc2f451823ccdfccd3bfd9a95b5e10e4484777d1ba7939f46f5487d", /* 204 */
    "6efba514602225570633f1315d6ef7c97448dee8c092da27641f8097bdeba958", /* 205 */
    "2d4f50ac084a69ff3a1cce2b2372dd170f7dd657b0a20c9e05aa6484a711d858", /* 206 */
    "a36c3c5221c0814c79f916f1a52af860c0031b434adc1315c8a3651d3652f730", /* 207 */
    "9a714762a2eaf898cde406c35c77b0fd50402c687128b20a9da2d2720e17558c", /* 208 */
    "15a0cf3a6fd5bd66382b7071ff1324b5902aba575b22d3025d210befa46e353c", /* 209 */
    "e6af2bce0e7731aaac7b5e6ce3b456fbd9520e495e3860e0eabdb2176f908402", /* 210 */
    "f2b17c5b5f8ca3dcccfe9b9fda9fc6276ed5a0a91159968778a1865d13a6c81e", /* 211 */
    "1e9f6cf9aa67ec813103ae89eaf838e88db1cca162a02c841f9ec1d3d723c1dd", /* 212 */
    "7ba4347526fb6bcc0c06a10b900df6ec287aab1894378af6033141da86d62683", /* 213 */
    "d3189a89565b700016dbc4953ce4edf429a11e8789993475eba110b853d710f0", /* 214 */
    "110be0edad072ef3666895bdc94ffdfbd9f5c84d3ed655539070a8be6d99b8b2", /* 215 */
    "b6ddc9689ceafc85f9d38579b098e104946467d6d7190c054c567a3923c59aa7", /* 216 */
    "1e8b064d6d568378c9b6538ab36b0996421a5342e21cd327545613333ef852b7", /* 217 */
    "11c22a53d556869c967ce8ba79dc4fb40de392f19bcada35574830a8ecc05f16", /* 218 */
    "6269109f8fd1efde8e95b5350c0bd8fe88a05d8e5a3600d0314e65f87cfc1e46", /* 219 */
    "f63270802ac57d6957a9559d63e9b0b00bb99cab1fd0ad0fa02da873a125b03c", /* 220 */
    "bc2a4e7628b4dec773e0a7a07fe46fce1ecefa98c253357ba95e04a7a28884e4", /* 221 */
    "184d48f136ab76ccd08fe4173cdda79f1c080e511379090092fffa0a648ff703", /* 222 */
    "32c57af2ed36d59e60ea71194a9525f47b51ee16df6e09dc730383c019aff6d2", /* 223 */
    "9fdb390851cf70c7c11a99550381ac0d9684cd29d6121f36156dbbeddb30d8e5", /* 224 */
    "f56c9b838af3f1e2f5cd53599e1f6e068ac2bc238260e2081d51ce608820d95d", /* 225 */
    "e7a79b20009e26b7b6a1a8941436a35292f3cdb4e3945ed1393ede1ae8473b92", /* 226 */
    "e008c466d1eb412db3c78b5e705aa062b9c470b0b680aea62c569a72b4a6dc65", /* 227 */
    "4f9001604492fc337ca1a7f9761d017358a871589c8d113441880e19a19d4ab9", /* 228 */
    "9c6f50373b55621141edacee661d1dcdb7d910e164aca8b95764d4319c16ae0c", /* 229 */
    "eaa852cf448ec62d409a53dc6d345c7ed17b6534c0c58d16fccbc7dae270acfb", /* 230 */
    "61dd6957c9cb82355e98c4446978b9c09bb0b0f1e62db7940cd74f3e316a5681", /* 231 */
    "7f0aaf941f863565f1c7ec86c398cd4d1cf6a1aea7d27005658047f280d5840d", /* 232 */
    "008b163c00c7da58649f747a3055fdb33950faf211dcead2181c1b5ecaeaf579", /* 233 */
    "b37b0d0c00b3c2ad82c81c2fce0fdaf5717366e5bab5a62477cbe7bd8e3a5ede", /* 234 */
    "265a04cdf6fb53fa1d3fea77942c4ae597d9ed558be2daf7d53fba914c3dbacd", /* 235 */
    "6de9d6c406cd81806b9aca512689cdcf959cc79297937be17eed8c46baae3413", /* 236 */
    "14e911c7640bf6aadc5a8e5184c9afc5bfd1dd4c7f6630f62c54642c670a8c2e", /* 237 */
    "fb45f9ed31ba21dd6e095c4808720705cd9d9549e2031b1508ce4282223437b2", /* 238 */
    "b0215cba71904909d354732454fb1703ce57f12c40be87df759bed0ebbe2869f", /* 239 */
    "2cb247c55ccfbebe039c13fc46ba2b2bec3975fe0ed93c6809d467748ef8658c", /* 240 */
    "fbf410195dac2c5d24a9200cb967fae28dad98bdf4439ccb0bddb87055542d82", /* 241 */
    "aa1bad447f4d4547d4f79859c9f4427dad3b0d920d541b024cc595b18da62e3c", /* 242 */
    "cc1551df750024116e7fdf1facf70a529bf96074687312d9131c30048dd3f32d", /* 243 */
    "472e16e7a4a434f955dd56c33b8f010eb5a50321f6dd0347d2e892267db253ce", /* 244 */
    "498184518a58649695e1a55b1e7d48a1110d761ce5559f537d19597b09dee017", /* 245 */
    "fb8afb47ea4620d653b2e2145bbd36771aab96102def9ba613dd29faa5fef941", /* 246 */
    "d29b60c74490beba783323e0769a96334cef1a528dc440cb09dcba61c18cd68e", /* 247 */
    "08da55bae2eeef7f6a86868b7449a2a4ae423f7fbb01e11997d971e3bfaa2df5", /* 248 */
    "2a91d614daf8ee0a44ce9a7406305811d7e987b168f47e4f3ac5d61be6f67307", /* 249 */
    "ac3a583f8e0f7370fd20c6f4de8467e1222dfc667947aea7a6dd880d024031fb", /* 250 */
    "eaec99494256b766411d309658ae3098152e773f2f290269d3d89ecefacc7c9f", /* 251 */
    "2e5251d0902b23a4046f42423e9a84f651db55e950bb4319c66e2b5bf0675590", /* 252 */
    "ead73b11ef79a4a06d051634c24120b32e502da7b0cbcd2d9c9257fda6e80c91", /* 253 */
    "49eca0a234f6eb3ff7cf0019056dd128d17ef22ea822fd837c6a3613cbc00c34", /* 254 */
    "88ef1143ab40897fcd2a271cbb5ff12db7068ecda597ec5bfcd1104da91aeb66", /* 255 */
    "e051c20c2cb90863f3f6b70a9aefe0169f1abeff45fc83f259aab5c15b49c823", /* 256 */
    "b57b9f40aed1213ec7bc5550457accb998ba5056805bcc9c765347127d31922b", /* 257 */
    "08d4b5027a544f19c4bd02018c6643091a666f2fe5a3865cc3307778df1db607", /* 258 */
    "aa6c694042bfff5faf2480a6cdf803aea3e69498d7b90fa3df60477a25b9eccf", /* 259 */
    "544e4c78a3856159f4ef414fe222bbda1c2234b0b1e9b4851947cf64da1556b9", /* 260 */
    "9bd4e99f03e43404019ec441820b1b719fd2ac6a995891a9a4bd725637cce477", /* 261 */
    "5ed213b05cbc6b19a169a247caf55611524659050b33ed2b0c1402005d899aa0", /* 262 */
    "9ce0d848ad785ad9ef97d4683019abef8a44654fe89f1f20e9b6a03ff2f42f58", /* 263 */
    "a7f43db01f7e6b68bf435226664f585acab26e242e76a8b63604359606ef901d", /* 264 */
    "0036c43994c478af420e6d5ebb8484f3f25781f761ec0b61a9a85fc36b41f20f", /* 265 */
    "1daf272eda3ac659c8bdbfb5c24cf79f0fc235f0a9b45093a887cf8fa38aca74", /* 266 */
    "76385cc20ae32fdaa844a05dcfa5af4c18c0e0d1cb67b23b40a92b44c96f57b4", /* 267 */
    "9b1713236416b0340d60fba6c1b45272cf7ce89215d651ae45ad0d023c6363a2", /* 268 */
    "94b9a52ff0f574fa4227658ddd81eb68887bc66db0848d8feb06125cf89d7aca", /* 269 */
    "5279e13ad9d2cd4d9b034713882f1724744fa429133c53e0648ca37c91d4e812", /* 270 */
    "f1af5e1c9c9f337d1f8681c34067210ab8bb86b232049e952bc49845f4d79d5e", /* 271 */
    "5c3d55543c53a0e8e4082f97a06365ff077445d65a96985e67526c5aeff87b87", /* 272 */
    "e12e274e760e8ff88852299ef908060aa2d55ba9b3d156845bcaecf70fe94678", /* 273 */
    "6f571c0574d3a137f905c2cb6de0d0935f43db252f565f802c61663d1204a8b1", /* 274 */
    "8bb8196269bb92e301aa27ab4c5935e4e27ca2e8f5a7cd7b605878ee2f41e8f2", /* 275 */
    "342c217d5d41ffd070a1f410e4b962da85a4e3191d08869efee9387b1a568712", /* 276 */
    "ab8c97b3ad3bb127742fdbc4884e9faf206257902e93b9483bcaa8c1ac5a3d35", /* 277 */
    "6818c0fe4bbeaa5364f0fb743bd96c78f2edf3463e0bc8d3e29fcb4a86891706", /* 278 */
    "5e7c2cbb509da0802275cf325288d7a76ea7055a309f5b845967545162eee2f6", /* 279 */
    "ccbe9f9051a154fce3c10250a859d64e6115484b7045efbfa4110289b7b9bc61", /* 280 */
    "a1ad624f6ffcc4f9d8bb86ad9f0888633a900474074ca10aa19dd404b81ddb90", /* 281 */
    "776600b23cf4aae50d3e6ad194f72f9b4cd437d8fd7473f940894b76ce4cd239", /* 282 */
    "b1b73193ab236f4ea7552c7a72906b90be4650cafb45b594aa6403521c33b6fb", /* 283 */
    "c8027b56c638f42b60daf9dd0fdbafe3ffd7c9836f89d0929d1152062a26f626", /* 284 */
    "cb421d9c09653efac4c75ac1a91449a060a719d12479f0287f3bfb6be8ef7ded", /* 285 */
    "c1937824ca09d41f5cf392bb2b6208af1d942e98d850ce0075dab6be39baf185", /* 286 */
    "03f17fd9866f985b823a1e457a2757ee999d3d6264113038551c1157c77ddec1", /* 287 */
    "312e9b6759aa845b25634dbabb991dd2d3bd45c31194d9003b3a48952c9af818", /* 288 */
    "eaf2ef5ba8807d2c29e5f689753a8cebe5b20eccc967918822fa25b0b8a6be9c", /* 289 */
    "b2e1d47d5c475d56651a8642297a7b98b1c26c1506be0d9e8305216cd453cbd4", /* 290 */
    "ad6bdac75d04098a578280dcc3d013771c45a5d666a42db86e53e1523b7d9e79", /* 291 */
    "b069ad2023ac53d5e745aeba159dd5a4235cfd953a2c6d4ee0ceca1055cb50d9", /* 292 */
    "0523674d3325f3d52ab27ed25e0f38d34091eb4ca92ea7316860c69c6bbb1741", /* 293 */
    "11c439b03c65617a2ce68e792c8c90556750b9b7937a5893ff0b15507f894c3f", /* 294 */
    "643a405f2d491729b21a2f32da56b1993ba68efed2f3448334112bd8d6f826ad", /* 295 */
    "5d66859deef7713e1b7cc962311a37fb45113dd8a83745bf3821d41bf2a47b89", /* 296 */
    "073210ff16f2a9e5cc4a6497f83edf391bf9fa1889c104266f7aa4f6d1a96b35", /* 297 */
    "835ac167751301bfff3dcc8140899b8fac425919a71004bea798eff1bfb1cd00", /* 298 */
    "b09b5f33f08bc831fc51f88d36fdf1cf28db88c0fa1574f9d1b016767e3342db", /* 299 */
    "b3809bdab490a15cf145409b9c055f4b093b6118ac349fa6817baedf103cac1b", /* 300 */
    "c3c38747ed207226ef8030573e6cde0c5f9205e9177a1e7396b162652e8042da", /* 301 */
    "2eb10f415219739956312fbcca57f9c955a5ff92157a8756b9bcda7b5b73bbc1", /* 302 */
    "0d5f8eea9ee9b284e22f8ffe5ff2d1e59b0aa49245f2b196988a172c584b17c6", /* 303 */
    "cdb85999a528a762062a6e5c93ad68e692c223e39deffa4dcc818ab8517499f1", /* 304 */
    "e972f912844492a6eedf5e9f16bd53bf314997f15b9e70458fa8b1776d2116a7", /* 305 */
    "4c72096ed674e19e0cb0b073e9f71f053290f7669b0962822d78b1e503d4c2d9", /* 306 */
    "1a765fbb2fc162078882ebe14b22981e89b0a20666981c2b6c73ab42f05b6cd5", /* 307 */
    "b4822edf440419d029e10179a421d841ff0310aa609378b7b859b10dabbf5db4", /* 308 */
    "b49d5aac9ea8f3a39345eefbb283e6441a873ef74389e7c298bbcfbd6758da4c", /* 309 */
    "6496fd3f8773ae665793ccf809b5796e915d48aa89f63a49cb664f45bea3beb8", /* 310 */
    "2fcd561488775243c5386ca65fa2d3526c79a6db63ffefee5689a1d68aa712b6", /* 311 */
    "ee3094e6ee0e4e28441812f5a7d8fb154b7cb103349af9c6dff96a5b7eae5c10", /* 312 */
    "fdbd1c1f8d6c41c13c8612cc750ed447f2316494d130e997ba641739ab5737e2", /* 313 */
    "cc59683377df3f2c5d69b0f3a0b522dc970f720440130db8bb1814ab964be5a3", /* 314 */
    "532f13981e68fe9abe6c9605183fa63436021b2437ed6e9a7c348e33724841b9", /* 315 */
    "601e59ce0e8bec37fe050211d663ae41694526d8b1e61e493b8af669d6a992d1", /* 316 */
    "3712131471987dd943ed98469849fe13a6c34c4ca6ea51ca5d0b669bbbda1c30", /* 317 */
    "75d62e3dd16c303df96072ebeabfea1c6931c55c7bce6f09ccef15bee03cb89d", /* 318 */
    "8fb7ac6a5842ff37e9caf96be0c708145eb8fecf9ac84c8e7e1faa2d80200806", /* 319 */
    "604937d1f47a19135de655361e1515348a4665f38a5f347a713b4c479c4c5fc9", /* 320 */
    /* END DIGEST TABLE */
};

static sha256_ctx_t s_ctx;
static uint8_t s_message[TABLE_LENGTHS];
static uint8_t s_million[1000000];

/* Byte i of every table message: the top byte of i * 2654435761 modulo 2^32. The same formula
 * as message_byte() in tests/python/test_sha256_table.py. */
static uint8_t message_byte(size_t i)
{
    return (uint8_t)(((uint32_t)i * 2654435761U) >> 24U);
}

static const char *at(const char *what, size_t value)
{
    static char text[64];

    (void)snprintf(text, sizeof(text), "%s %lu", what, (unsigned long)value);
    return text;
}

static const char *at2(const char *what, size_t first, size_t second)
{
    static char text[64];

    (void)snprintf(text, sizeof(text), "%s %lu %lu", what, (unsigned long)first,
                   (unsigned long)second);
    return text;
}

static uint8_t hex_digit(char c)
{
    uint8_t value = 0U;

    if ((c >= '0') && (c <= '9')) {
        value = (uint8_t)(c - '0');
    } else if ((c >= 'a') && (c <= 'f')) {
        value = (uint8_t)(10 + (c - 'a'));
    } else {
        TEST_FAIL_MESSAGE("not a lower-case hexadecimal digit");
    }

    return value;
}

/* 64 hexadecimal digits to the 32 bytes they write. */
static void from_hex(const char *hex, uint8_t digest[SHA256_DIGEST_SIZE])
{
    TEST_ASSERT_EQUAL_size_t(2U * SHA256_DIGEST_SIZE, strlen(hex));

    for (size_t i = 0U; i < SHA256_DIGEST_SIZE; i++) {
        digest[i] = (uint8_t)((hex_digit(hex[2U * i]) << 4U) | hex_digit(hex[(2U * i) + 1U]));
    }
}

static void expect_digest(const char *hex, const uint8_t digest[SHA256_DIGEST_SIZE],
                          const char *message)
{
    uint8_t expected[SHA256_DIGEST_SIZE];

    from_hex(hex, expected);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(expected, digest, SHA256_DIGEST_SIZE, message);
}

/* The digest of `len` bytes at `data` with init, one update and final. */
static void digest_of(const uint8_t *data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE])
{
    TEST_ASSERT_TRUE(sha256_init(&s_ctx));
    TEST_ASSERT_TRUE(sha256_update(&s_ctx, data, len));
    TEST_ASSERT_TRUE(sha256_final(&s_ctx, digest));
}

/* The members of two contexts are equal (the struct may have padding, so no memcmp). */
static void expect_same_context(const sha256_ctx_t *expected, const sha256_ctx_t *actual)
{
    TEST_ASSERT_EQUAL_HEX32_ARRAY(expected->state, actual->state, 8U);
    TEST_ASSERT_EQUAL_UINT64(expected->bytes, actual->bytes);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected->block, actual->block, SHA256_BLOCK_SIZE);
    TEST_ASSERT_EQUAL_UINT32(expected->fill, actual->fill);
}

void setUp(void)
{
    /* Nonsense first: sha256_init() must need nothing from the memory it is given. */
    (void)memset(&s_ctx, 0xA5, sizeof(s_ctx));

    for (size_t i = 0U; i < TABLE_LENGTHS; i++) {
        s_message[i] = message_byte(i);
    }
}

void tearDown(void)
{
}

/* ------------------------------------------------------------------------------------------
 * The FIPS 180-4 examples
 * ---------------------------------------------------------------------------------------- */

static void test_fips_example_one_block_abc(void)
{
    static const uint8_t abc[3] = {0x61U, 0x62U, 0x63U};
    uint8_t digest[SHA256_DIGEST_SIZE];

    digest_of(abc, sizeof(abc), digest);
    expect_digest("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", digest,
                  "abc");
}

/* 56 bytes: the message fills the block up to where the length would go, so the padding takes
 * a second block. */
static void test_fips_example_two_blocks(void)
{
    static const char text[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t digest[SHA256_DIGEST_SIZE];

    TEST_ASSERT_EQUAL_size_t(56U, strlen(text));
    digest_of((const uint8_t *)text, strlen(text), digest);
    expect_digest("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", digest,
                  "two blocks");
}

/* One million "a", as one update and as 1000 updates of 1000 bytes (which do not end on block
 * boundaries: 1000 = 15 * 64 + 40). */
static void test_fips_example_one_million_a(void)
{
    static const char *const expected =
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    uint8_t digest[SHA256_DIGEST_SIZE];

    (void)memset(s_million, 0x61, sizeof(s_million));
    digest_of(s_million, sizeof(s_million), digest);
    expect_digest(expected, digest, "one update");

    TEST_ASSERT_TRUE(sha256_init(&s_ctx));

    for (size_t i = 0U; i < 1000U; i++) {
        TEST_ASSERT_TRUE(sha256_update(&s_ctx, &s_million[1000U * i], 1000U));
    }

    TEST_ASSERT_TRUE(sha256_final(&s_ctx, digest));
    expect_digest(expected, digest, "1000 updates");
}

/* ------------------------------------------------------------------------------------------
 * Every length, every split
 * ---------------------------------------------------------------------------------------- */

static void test_every_length_from_0_to_320_bytes_matches_hashlib(void)
{
    uint8_t digest[SHA256_DIGEST_SIZE];

    for (size_t n = 0U; n < TABLE_LENGTHS; n++) {
        digest_of(s_message, n, digest);
        expect_digest(s_digests[n], digest, at("length", n));
    }
}

/* The message given as message[0..split) and message[split..n), for every n and every split,
 * the empty pieces included. A buffer that is not taken over correctly from one update to the
 * next, or a block compressed straight from the input at the wrong moment, shows up here. */
static void test_every_split_into_two_updates_gives_the_same_digest(void)
{
    uint8_t digest[SHA256_DIGEST_SIZE];

    for (size_t n = 0U; n < TABLE_LENGTHS; n++) {
        for (size_t split = 0U; split <= n; split++) {
            TEST_ASSERT_TRUE(sha256_init(&s_ctx));
            TEST_ASSERT_TRUE(sha256_update(&s_ctx, s_message, split));
            TEST_ASSERT_TRUE(sha256_update(&s_ctx, &s_message[split], n - split));
            TEST_ASSERT_TRUE(sha256_final(&s_ctx, digest));
            expect_digest(s_digests[n], digest, at2("length, split", n, split));
        }
    }
}

static void test_one_byte_per_update_gives_the_same_digest(void)
{
    uint8_t digest[SHA256_DIGEST_SIZE];

    for (size_t n = 0U; n < TABLE_LENGTHS; n++) {
        TEST_ASSERT_TRUE(sha256_init(&s_ctx));

        for (size_t i = 0U; i < n; i++) {
            TEST_ASSERT_TRUE(sha256_update(&s_ctx, &s_message[i], 1U));
        }

        TEST_ASSERT_TRUE(sha256_final(&s_ctx, digest));
        expect_digest(s_digests[n], digest, at("length", n));
    }
}

/* Empty updates anywhere, NULL among them, change nothing. */
static void test_empty_updates_change_nothing(void)
{
    uint8_t digest[SHA256_DIGEST_SIZE];

    TEST_ASSERT_TRUE(sha256_init(&s_ctx));
    TEST_ASSERT_TRUE(sha256_update(&s_ctx, NULL, 0U));
    TEST_ASSERT_TRUE(sha256_update(&s_ctx, s_message, 100U));
    TEST_ASSERT_TRUE(sha256_update(&s_ctx, s_message, 0U));
    TEST_ASSERT_TRUE(sha256_update(&s_ctx, NULL, 0U));
    TEST_ASSERT_TRUE(sha256_update(&s_ctx, &s_message[100], 100U));
    TEST_ASSERT_TRUE(sha256_final(&s_ctx, digest));
    expect_digest(s_digests[200], digest, "200 bytes with empty updates");
}

/* ------------------------------------------------------------------------------------------
 * Padding at the block boundaries
 * ---------------------------------------------------------------------------------------- */

/*
 * FIPS 180-4 section 5.1.1: the message, a 1 bit, zeros up to 448 bits modulo 512, and the
 * 64-bit length. In bytes, for a message of L bytes in its last block:
 *
 *   L mod 64   blocks of padding   why
 *   55         0 extra             55 + 1 + 8 = 64: the 0x80 and the length just fit
 *   56         1 extra             56 + 1 > 56: the length goes into a block of its own
 *   63         1 extra             the 0x80 is the last byte of the message's block
 *   0 (L > 0)  1 extra             the message ends on a boundary: a block of padding alone
 *
 * Each boundary is checked in the first and in the second block (55, 56, 63, 64 and 119, 120,
 * 127, 128), as one update and as a message that reaches the boundary only in sha256_final()'s
 * buffer (all but the last byte in one update, the last byte in another).
 */
static void test_padding_at_the_55_56_63_64_byte_boundaries(void)
{
    static const size_t lengths[] = {55U, 56U, 63U, 64U, 119U, 120U, 127U, 128U};
    uint8_t digest[SHA256_DIGEST_SIZE];

    for (size_t i = 0U; i < (sizeof(lengths) / sizeof(lengths[0])); i++) {
        const size_t n = lengths[i];

        digest_of(s_message, n, digest);
        expect_digest(s_digests[n], digest, at("one update, length", n));

        TEST_ASSERT_TRUE(sha256_init(&s_ctx));
        TEST_ASSERT_TRUE(sha256_update(&s_ctx, s_message, n - 1U));
        TEST_ASSERT_TRUE(sha256_update(&s_ctx, &s_message[n - 1U], 1U));
        TEST_ASSERT_TRUE(sha256_final(&s_ctx, digest));
        expect_digest(s_digests[n], digest, at("two updates, length", n));
    }
}

/* sha256_final() writes every byte of the padding itself: a buffer whose unused bytes are not
 * 0 (here 0x5A, written into the private buffer after init) gives the same digests. An
 * implementation that relied on the zeros that sha256_init() leaves would pass every other test
 * of this file. One case per fill count of the last block. */
static void test_padding_does_not_depend_on_what_the_buffer_held_before(void)
{
    uint8_t digest[SHA256_DIGEST_SIZE];

    for (size_t n = 0U; n < 128U; n++) {
        TEST_ASSERT_TRUE(sha256_init(&s_ctx));
        (void)memset(s_ctx.block, 0x5A, sizeof(s_ctx.block));
        TEST_ASSERT_TRUE(sha256_update(&s_ctx, s_message, n));
        TEST_ASSERT_TRUE(sha256_final(&s_ctx, digest));
        expect_digest(s_digests[n], digest, at("length", n));
    }
}

/* ------------------------------------------------------------------------------------------
 * The context
 * ---------------------------------------------------------------------------------------- */

/* sha256_final() leaves a context as sha256_init() would, so the next update starts a new
 * message; setUp() filled the memory with 0xA5 before the first init. */
static void test_final_starts_the_next_message(void)
{
    sha256_ctx_t fresh;
    uint8_t digest[SHA256_DIGEST_SIZE];

    TEST_ASSERT_TRUE(sha256_init(&fresh));
    digest_of(s_message, 300U, digest);
    expect_same_context(&fresh, &s_ctx);

    TEST_ASSERT_TRUE(sha256_update(&s_ctx, s_message, 3U));
    TEST_ASSERT_TRUE(sha256_final(&s_ctx, digest));
    expect_digest(s_digests[3], digest, "message after a final");
}

/* The length goes into the padding with all 64 bits: two contexts that differ only in a high
 * bit of the byte count give different digests. 2^32 bytes and more cannot be hashed in a unit
 * test, so the count is set directly; adding multiples of 64 keeps the fill count consistent.
 * 2^61 bytes are 2^64 bits, which the 64-bit length field cannot tell from 0 (sha256.h,
 * LENGTH). */
static void test_the_length_counter_uses_all_64_bits(void)
{
    static const uint64_t added[] = {(uint64_t)1U << 29U, (uint64_t)1U << 32U, (uint64_t)1U << 40U,
                                     (uint64_t)1U << 56U, (uint64_t)1U << 60U};
    uint8_t base[SHA256_DIGEST_SIZE];
    uint8_t others[sizeof(added) / sizeof(added[0])][SHA256_DIGEST_SIZE];
    sha256_ctx_t copy;

    TEST_ASSERT_EQUAL_size_t(8U, sizeof(s_ctx.bytes));
    TEST_ASSERT_TRUE(sha256_init(&s_ctx));
    TEST_ASSERT_TRUE(sha256_update(&s_ctx, s_message, 5U));
    copy = s_ctx;
    TEST_ASSERT_TRUE(sha256_final(&s_ctx, base));

    for (size_t i = 0U; i < (sizeof(added) / sizeof(added[0])); i++) {
        s_ctx = copy;
        s_ctx.bytes += added[i];
        TEST_ASSERT_TRUE(sha256_final(&s_ctx, others[i]));
        TEST_ASSERT_FALSE_MESSAGE(memcmp(base, others[i], SHA256_DIGEST_SIZE) == 0, at("bit", i));

        for (size_t j = 0U; j < i; j++) {
            TEST_ASSERT_FALSE_MESSAGE(memcmp(others[j], others[i], SHA256_DIGEST_SIZE) == 0,
                                      at2("bits", j, i));
        }
    }

    /* 2^61 bytes: the bit count wraps to the same 64-bit value. */
    s_ctx = copy;
    s_ctx.bytes += (uint64_t)1U << 61U;
    TEST_ASSERT_TRUE(sha256_final(&s_ctx, others[0]));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(base, others[0], SHA256_DIGEST_SIZE);
}

static void test_refused_calls_change_nothing(void)
{
    sha256_ctx_t before;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t untouched[SHA256_DIGEST_SIZE];

    TEST_ASSERT_FALSE(sha256_init(NULL));
    TEST_ASSERT_FALSE(sha256_update(NULL, s_message, 1U));
    TEST_ASSERT_FALSE(sha256_final(NULL, digest));

    TEST_ASSERT_TRUE(sha256_init(&s_ctx));
    TEST_ASSERT_TRUE(sha256_update(&s_ctx, s_message, 10U));
    before = s_ctx;

    TEST_ASSERT_FALSE(sha256_update(&s_ctx, NULL, 1U));
    expect_same_context(&before, &s_ctx);
    TEST_ASSERT_FALSE(sha256_final(&s_ctx, NULL));
    expect_same_context(&before, &s_ctx);

    /* A context whose fill count is beyond a block was not prepared by sha256_init(). */
    s_ctx.fill = SHA256_BLOCK_SIZE;
    before = s_ctx;
    (void)memset(digest, 0x11, sizeof(digest));
    (void)memset(untouched, 0x11, sizeof(untouched));
    TEST_ASSERT_FALSE(sha256_update(&s_ctx, s_message, 1U));
    TEST_ASSERT_FALSE(sha256_final(&s_ctx, digest));
    expect_same_context(&before, &s_ctx);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(untouched, digest, SHA256_DIGEST_SIZE);

    /* The largest fill count a prepared context can have is accepted. */
    TEST_ASSERT_TRUE(sha256_init(&s_ctx));
    TEST_ASSERT_TRUE(sha256_update(&s_ctx, s_message, SHA256_BLOCK_SIZE - 1U));
    TEST_ASSERT_TRUE(sha256_update(&s_ctx, &s_message[SHA256_BLOCK_SIZE - 1U], 0U));
    TEST_ASSERT_TRUE(sha256_final(&s_ctx, digest));
    expect_digest(s_digests[SHA256_BLOCK_SIZE - 1U], digest, "63 bytes");
}

static void test_compute_equals_init_update_final(void)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t untouched[SHA256_DIGEST_SIZE];

    for (size_t n = 0U; n < TABLE_LENGTHS; n += 7U) {
        TEST_ASSERT_TRUE(sha256_compute(s_message, n, digest));
        expect_digest(s_digests[n], digest, at("length", n));
    }

    TEST_ASSERT_TRUE(sha256_compute(NULL, 0U, digest));
    expect_digest(s_digests[0], digest, "NULL, 0");

    (void)memset(digest, 0x22, sizeof(digest));
    (void)memset(untouched, 0x22, sizeof(untouched));
    TEST_ASSERT_FALSE(sha256_compute(NULL, 1U, digest));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(untouched, digest, SHA256_DIGEST_SIZE);
    TEST_ASSERT_FALSE(sha256_compute(s_message, 1U, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fips_example_one_block_abc);
    RUN_TEST(test_fips_example_two_blocks);
    RUN_TEST(test_fips_example_one_million_a);
    RUN_TEST(test_every_length_from_0_to_320_bytes_matches_hashlib);
    RUN_TEST(test_every_split_into_two_updates_gives_the_same_digest);
    RUN_TEST(test_one_byte_per_update_gives_the_same_digest);
    RUN_TEST(test_empty_updates_change_nothing);
    RUN_TEST(test_padding_at_the_55_56_63_64_byte_boundaries);
    RUN_TEST(test_padding_does_not_depend_on_what_the_buffer_held_before);
    RUN_TEST(test_final_starts_the_next_message);
    RUN_TEST(test_the_length_counter_uses_all_64_bits);
    RUN_TEST(test_refused_calls_change_nothing);
    RUN_TEST(test_compute_equals_init_update_final);
    return UNITY_END();
}
