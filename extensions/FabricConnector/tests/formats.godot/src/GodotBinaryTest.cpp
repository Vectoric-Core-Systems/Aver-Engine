// GodotBinaryTest -- Godot 4 binary resources (RSRC / RSCC) and the Zstandard decoder behind them (modules/formats.godot).
//
// Every binary file here is built by the test (GodotBinaryFixtures.hpp), field by field, from the format description in GodotBinary.hpp; none is a
// copy of a real file. The Zstandard frames are the one pasted-in data: small compressed samples of synthetic text (made with a reference
// compressor at several levels), checked against generators written out below. Then a hostile-input sweep: every prefix of every fixture, every
// byte flipped, and hand-crafted lies about counts, sizes, offsets and nesting. Pure data: no device, no engine. Exits 0 or 1.
#include "GodotBinaryFixtures.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/godot/GodotBinary.hpp"
#include "aver/formats/godot/GodotProjectFs.hpp"
#include "aver/formats/godot/GodotZstd.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>

using namespace aver;
using namespace aver::fmt::godot;
using namespace gbfix;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool near(f64 a, f64 b, f64 eps = 1e-6) { return std::fabs(a - b) <= eps; }

static bool numsAre(const GodotVariant* v, std::initializer_list<f64> want, f64 eps = 1e-6) {
    if (!v || v->nums.size() != want.size()) return false;
    usize i = 0;
    for (f64 w : want)
        if (!near(v->nums[i++], w, eps)) return false;
    return true;
}

static bool hasDiag(const GodotDiagnostics& d, GodotSeverity sev, const std::string& textPart) {
    for (const GodotDiag& x : d.items)
        if (x.severity == sev && x.message.find(textPart) != std::string::npos) return true;
    return false;
}

static std::string firstError(const GodotDiagnostics& d) {
    for (const GodotDiag& x : d.items)
        if (x.severity == GodotSeverity::Error) return x.message;
    return {};
}

static bool parseRes(const Bytes& b, GodotResource& r, GodotDiagnostics& d) { return parseGodotBinaryResource(b.data(), b.size(), "res://t.res", r, d); }
static bool parseScn(const Bytes& b, GodotScene& s, GodotDiagnostics& d) { return parseGodotBinaryScene(b.data(), b.size(), "res://t.scn", s, d); }

// ---- Zstandard ----------------------------------------------------------------------------------------------------------------

static Bytes fromHex(const char* hex) {
    Bytes out;
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (usize i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(u8(nib(hex[i]) * 16 + nib(hex[i + 1])));
    return out;
}

// The generators behind the samples: a 31-bit linear congruential sequence.
struct Lcg {
    u32 s;
    explicit Lcg(u32 seed) : s(seed) {}
    u32 next() {
        s = (s * 1103515245u + 12345u) & 0x7FFFFFFFu;
        return s;
    }
};

static Bytes genText(u32 seed, usize len) {
    static const char* const words[16] = {"alpha", "beta", "gamma", "delta", "mesh", "shape", "node", "scene", "tile", "grid", "item", "cell", "wall", "floor", "ramp", "trunk"};
    Lcg r(seed);
    Bytes out;
    while (out.size() < len) {
        const u32 s = r.next();
        const std::string w = std::string(words[(s >> 16) & 15]) + ((((s >> 8) & 7) == 0) ? ".\n" : " ");
        out.insert(out.end(), w.begin(), w.end());
    }
    out.resize(len);
    return out;
}
static Bytes genNoise(u32 seed, usize len) {
    Lcg r(seed);
    Bytes out(len);
    for (usize i = 0; i < len; ++i) out[i] = u8((r.next() >> 16) & 0xFF);
    return out;
}
static Bytes genRun() {
    Bytes out(3000, 0x61);
    for (char c : std::string("tail")) out.push_back(u8(c));
    return out;
}
static Bytes genSkew(u32 seed, usize len) {
    static const int table[18] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    Lcg r(seed);
    Bytes out(len);
    for (usize i = 0; i < len; ++i) out[i] = u8(40 + table[(r.next() >> 16) % 18]);
    return out;
}
static Bytes genWide(u32 seed, usize len) {
    Lcg r(seed);
    Bytes out(len);
    for (usize i = 0; i < len; ++i) {
        const u32 v = (r.next() >> 16) & 0xFF;
        out[i] = u8(v < 128 ? (v & 3) : (v & 0x7F));
    }
    return out;
}
static Bytes genNibbles(u32 seed, usize len) {   // sixteen symbols, the smaller more likely: the Huffman weights fit in the direct 4-bit form
    Lcg r(seed);
    Bytes out(len);
    for (usize i = 0; i < len; ++i) {
        const u32 a = (r.next() >> 16) & 15, b = (r.next() >> 16) & 15;
        out[i] = u8(a < b ? a : b);
    }
    return out;
}
static Bytes genPeriodic(usize len) {
    Bytes out(len);
    for (usize i = 0; i < len; ++i) {
        const usize j = i % 300;
        out[i] = u8((j * 7 + j / 13) & 0xFF);
    }
    return out;
}

// Frames made by a reference compressor from the generators above. text3 carries a content checksum; noise is stored raw; run is RLE; skew and
// wide are Huffman literals (wide's weight table is itself FSE-coded, nibbles' is stored directly); periodic runs over three 128 KiB blocks with repeat offsets.
static const char* const kZText1 =
    "28b52ffd60b003bd0b0042491915a015b40df024d94de45337f269dba8aae7b36fee28260e8e21bab32ec7860f5487a9eec754e72ca42ec786df419d001958e40f6475c21cd568988983633f2613c7ea2cc4726ca8db521e60880c506714b7ce6c509c3a44771aaa62a813c76a4671edc71480b1a861074992a430ec1b204284a03c0f1010ab4992a403aac648e3e4331d839e8f420ca73043c7fb046304fa8d03a975c5b9e0a59e3a54dfd3f2327c810da244e13a010be743544ad83f72d2e0fa81065c7261679c0b3a7f13270e6c630614050a9b70d81982567a4ad7c1fb235cec2c80c6c67bed34f041ecbb593954c7fe87b585cc08a80e8b4fed5497a6144f8ccc25f2469ab4b2f73e6d03c7a297a01a2096c04e50e7e189b550dbb8475eec273ca4882f5c3a5938e83701fca5fadadb461394213d75a321df3a5a1dbf5a48b3becf4b3ad6d875288a6f3fa7ce8fde3f16afacd677c46effb6a191a58413254574ad096c252045d0cfd4a7a5a25edf5f822a40dcd6b0151839da47ab333cab";
static const char* const kZText3 =
    "28b52ffd64b0035d0b0062c71415b0a5396089606f22cdf7b69676a3312c32a633a003a5699aa63f8ea169b30ff4c01050eaada4d2f7540861754fa5371f8a7e1cd35190a0343ac702ad74004df7449182ae641fadbb12d0a4f895b84235ab4280a3a851cf822449fb1b2042a498bb6e10204a496194c6309420d52047aeec6248bac43e6d733cf03bc5789be90e2564dd1e3152b65b71a9774e66804e7424bfc410397611fc0301704e1c48c97aa11b7509aca0b8e6c6376b78af279bcc88d8504e8c215a0a5dba18b72ac867244abb90171a07cc2a1cd81816becb41ee775d718918674023b10c0f7deaee368c41ed6700f627e64606624699a6a77f7dcc267a98d5ef08485318f27a07934ca3d113c808303f9a0229fd0850de14d89e953c73407424e1612267df50adf22bc618a4fa0598f5a0a278f5b0fc41f6e153c034135640fedc70d48911f57ed338b58f11dc24edd6d5b10661c27d4ecf1f0d1a250d833d9e27627c5730e181e21d91224ca8fa19e755dcb4ae5d";
static const char* const kZText19 =
    "28b52ffd60c4085d120072c61212a0bb0120693689615a864dff1fb6f7cd1305c22c828a8a8adabd0f470f0c15352f12ca83dc6aa8db0e50fbcd6ce38b54bd0b95f92199f7e5d51696c22c9267d3b2b077ae9478e416c51b8139a8b1072964677f0321080146a1bc853d115014a68c245569032dc61719ece180bb414c02a8c818a4f0db53915d86db313a33dddeb05cf2eceadadbe44b7fb0eaff6cc3aa4fb4fbc449915daf2c0a9135808e09e415f0c23a54f64bf92cc910eca0d89e0678fcb74be69c9d64fe4c67ec73bdfc0f8bd223b9c211268e9224290d0642ed5626629d8d6a3888f4b2d6999b1ebd4cf444042846ce09b5c2865f3d5f3b204e8cdc8b5d08c7e346f74363ff1b20ba7f6c96253d8f44eb3f89ee3a18f8cfa0431ce777fb0541b472c2b51e7eccb2f84fa79ffa49f7ff2768331f089b638a51115e9a6b67b3c6404993f247739d1b27bce838496af0c2f80ebe314f41b25817913f64255e6366e5349675414c783026f02abdf78fb38ce34a1d9213d55abec2c09bb816dd128e429a1acb0b04ccccfe7894263efcc1ae180c61a85f02c0a77ca821c06bb1e54fde206ac8e349b9dea70c8d047034a8f2514480b9c341c2970528d548f3bc0486a7ea853394eedf0bcf828028b8bae7cb906ab87178d91c39d5196c64fc1bbec1b9b9ccd9d05fc39b7fd46ac0df71760a3ed73673926488b15721b01347168b9ee7222f52429e3b744cc64fc832702c461121a376c2bd87865505342dd6054c01d1df6fd91d15aac8c96fe111d649f3525ec189b384273a56dcee1250a1f90485811dc55df303ff8eaa9a0cf09c322c09ac0a";
static const char* const kZNoise =
    "28b52ffd602c00610900df097885d16cb8733c681dacfd15e95946d944626f3deaa9808d92097f3122117ace9480531ca6804e8644a35c83b69e25b789c8d7c83f50b101b6414ea95837b22108df16c0c730736f2a6bd0037cd34c5db22ceb4413f620fda4661eaf5529df7aecd8f151b539039be63b338ed4b01546d8c883a7ff5127f875b1c93caf9f59505aa4bcc606555783924d5c1a56edd7e713d377ec9b1b1d6b40582647fe50791b687b4f17533ac59f44d823a29a41eaba7b849f087f0a5acd1f940dc9dcf695542f95833d33a1a7661413601c3766a6372715d4f543d73697289d57b9df470772c62a2e30404354501c38ec80c61263d530bcedab7e3b0a4074abda0f9ffd26ee46722ae710d624d6727d9dc6deff7a0cadb2c221c8ec2c411af570c4b6ce4a3fc7a54e5a3b136f6f2f1b";
static const char* const kZRun = "28b52ffd60bc0a6d00003061617461696c0100b3f30116";
static const char* const kZSkew =
    "28b52ffd60dc048516006a4c9c0809a0edf0db6d84ff4721860083008600d1a9b0334f2891c237da0120e6a00aef065c7f2a0701d17558526aab99a2515177a624dc2c64eda668897d6c993ba9e06566370a32416662803a146b565b113114e14bc8e00689e845d2731a633229718a7c18770a387f01a21701c63ca0f4803c7fcb52fca00bfd8e56a80d6d1ac6009d149606f858cfb095b6523578930f1d7a30bac14517207770dee9f700c144510105331b67735968ccdcda366daf2df2db694e9abd1f375797a2e44a144bd83f3f913e8e0d52301700f962490a2d953d902d18ee4480594987302d94a207cb5c781af1a54e079567adad12f5b3615eb45b0dbb0e6228e81dfd815c8b03f26a5d6186a18f1b00c05cd5b31e2747e148101adb9b38819511460a4ffb784530d350cd569f1cb45e05026a40791bb7d1efa93908c40ca0374226808a18ba4aaa2030d7f203da7ef03aafd60968428bdf8860f2d6de013516b2e4d0b22d614448dccb2513977db1582eddd58af5bab96926d07d1404de527483c01a711980d8698d66ac92a4825da7d3fed20e965c6ea15dc826bd2b28127c379b1790e60d33c40c57b546f36e56456c82188be8cc1b04ec50495c12b0804ba1dd29b0576157179857dd3426b614925e81ea026de3d724207920aec2818ed7031c5a0c573d22ee2e594cab4308786169a60ecc7af5973dea185ecaa41640e40e81f6f2af86420009e1914042a5b52a1101407dc059faeabd6a8f0fa84a5597549334a5b831d88cb5465de93a0903cd0543a87108811431cc08143266822111290b73200242884190f10110e01e8c45a099e3bc8ea38d7788d30c9f7fac1083612388654df41635fc840d574bec90fb109f8b7ab50c71e7e9d0cebd17169a1647413342a00c0ecd63bd75fd4aa24a16269b432e396d435adf706d17545590c1cebd120c73ca2a63ac33fef7a522622e0ef260168f51aa76fe3e1e828f099193734f6f56fb6e7fcae3fcd50ef194346f98b91e8a8fab";
static const char* const kZWide =
    "28b52ffd60dc04252100fa5c5010212014be1c9c4570acbf0d4919bfbe08094b978c588e978cb3f967bf5d1f36a59402fb000601fa00af8ec4c687fafd69ab45a963446a5a78ed8e6fcf6149a4e8e4dd8bab2ff3684b71adf598253732494580ed326bfe5c4271e6394bd1695567cab73113ed482a0aae802c494daf44c1d2b474bd751a3a8115825f31f9f53b2f242993b161d170180f980d5c221acc9149916b2bfadfc89fa86fbd273473ad1b38ee8b3d900e6a9f221a44469bbe1b957575c4115990e24302881e2cc6028d09d0a8b81553193dec1dc3e317a9eec053662865bca9a26460473bc47af2e973a9447c4b6c90667f858f941c820ad05cda93fc87f433f81630361aff92531a4947f01ad911958fae93625ab8600a3869c23e7081851e3c217d777e4daf855efb402eae03eee5da9e70f753debbb30a742145c47f8274332b2d4bba68230a9689bd5c25d69909089381fa548a439e49ab4a12a108f9c1d6d00eee2f3b09130ac71f686637eacd40cc56f8951cbb15c019aae92b804711aca8561afee6200a7e1254028bc508063fb1ed3f594b83e70ae9c131cea0fb3c4deb9c028bbf07bf8503b3fbe45a29550e7a7c217d004417cbde308d344c02af6f79ce5086aabfe4d6bc2420a9a3944d3c21ee70e925ec65ea07bc0d40c93104269e485493040e47853f21f4ebf8c2cf342b0274138d3c561e6069d6fae494fc038323f2d94d2f6d36f90adc4c8256a531e1ed09da8ff2049123a94e40eee4cca2a643ff489a2dc39d2262e728385e1e6603ea1e45ea138b3d3d720fa607b96197da46cc8b61058921e21b3a385e196fde9d2a11291c917e2bfc5b95e8113b6c0b2652ec8ee5622411c80149a00cda64a0ce5b5d1073d7722a88de466db8284b615abbcfb67d08a3cca10fa239d6408b64608079f1095a66c7e98145681eedc41a745bdc72e95313c32d7a6b5373cf1f3b81d0f7bd899b1f8a09038ff0c32d26b9a3ee44762d3efabfdc75f576c3e1bf5a284976e4f42ef270f4423727c41c4d6d47dd63001dd963063604650cdfdf8f53156aa907a18490738c70c1db3002e0ee4d7745bdc43f7d566e5d86241f82154033bfdef3257301ad133beef29ae040b5ce9b8d5630c56bfd453d98da024da9eb09ccbe17c0f1f38404b5e784f579a83d6d31df0b0dd59a07cb380f7b0d62be94765a25b68b0d9d09f4eea92d861023c0d9fa99f3566d051ddc105906640062b48d47f8ff6572aa2fce9433e0bd98791e36a5e5c93bcdc89c1eeefa2cbe61a62350a8214cea06ad8939d527c7ff6ad1d7529da5df9203a83a3bb874d6d23b9f435d6a4aa2b596245571f98e3422c54a2b460ec808fe3fee1e55426e30dd5e71a733fe8d0f6f7adbd9e23e609246184ca1af04e4879e74713bb4f70c4dac0bd7309e8c4a0e4da0b54957fd1f9df94129978afadd8082fcfb1611ceb57fe924522bd0989598757d6236f04df5b8fd1d03007392c5757171054390a9";
static const char* const kZNibbles =
    "28b52ffd602002b50c00466f5d8e55545444443332105b0058005a005229c79499cbbc84c807177e997b799f6078747652fbe4464043771564c45add4b4420323413c86237bf30336ab049ccd9619c99fcd1e10c81c4a7184175852fac426be4a4f48b64333232401e7ab250b51205214172da1304f51d2d4bf5593a3842de90823377a9f843dedcd7d921550d2caeb8b13da971230f84bf50011e12f63c6624827cb2dcc7470f700eca6279c6fc0a56529bdc4dc0a9eb9ada7b42be83c58b3f17590662fdd494e7b40043214e91010c752914a91a81876b7d6b5fb119722f2ee0aa07c3111389a2b59b5d8dfc3ebd523e49248d40f42f0a008e333e1b536da8cb297ec5aa3aa9660da6d3463c468127505d00e5162317e81bfe3066de5e0a25d2dbfc1ee00eaa200ec143262b26cd404f20525353f06537d0a076e8ca7abecd5e2a50dea9f8777921292b831fe18ad063abc45fe2146f1c0523504d8d952898d1255c9916a200e418d01a935f46cf36c9af1a37250f6a0fa7aa841eb8588ca32f030b04015ca2649ab8f288c4d812bda0aa12512a9cd89ed998b9400d1a5ba2";
static const char* const kZPeriodic =
    "28b52ffda0e09304006c0800640f00070e151c232a31383f464d545c636a71787f868d949ba2a9b0b8bfc6cdd4dbe2e9f0f7fe050c141b222930373e454c535a616870777e858c939aa1a8afb6bdc4ccd3dae1e8eff6fd040b121920282f363d444b525960676e757c848b9299a0a7aeb5bcc3cad1d8e0e7eef5fc030a11181f262d343c434a51585f666d747b828990989fa6adb4bbc2c9d0d7dee5ecf4fb020910171e252c333a414850575e656c737a81888f969da4acb3bac1c8cfd6dde4ebf2f900080f161d242b323940474e555c646b727980878e959ca3aab1b8c0c7ced5dce3eaf1f8ff060d145b626970b7bec5cc131a21286f767d84cbd2d9e0272e353c440700d1fe2f2b6ba009280414020a8142badcc2480144000008840100fcff0e844c0000081b0100fc7f3910024d000008be0100dc131d0801";

// The scene fixture's stream (buildScene(false, false).d.build(false)), compressed at level 9.
static const char* const kZScene =
    "28b52ffd602d04cd110066174f3b506b93c630d32514cd181b60666666966b06ccd05bdbf6d8dd1e7ffb37b9097781d9ee3953b845ec7c7088d51b5bd0275783329bb111bb216924053b003f0041001d360cab9d95b9d0592c18abf2a778dcf435f8a1d62f99fa68e64208958a56700b394eb0c021c2a0a1e39756a895a629858a61fec91ff9237e880f3fe5f61c27e372a12d4a98316606fc095eed3038114dd34f7f830bff2fb1b698ab731988166cc116673502477460b1603c56c5f713d06514830c1e300f3e04f013aba52d028826c7f7947354a0e28a03ef84cc4906f2a85e364c3524c34cdd906ceaa3dc3ad0827135dd55e9f1c80d0e9d978ba714d3e87464dc2c647256900519194d461a46b7524190c82d7d149baea9143d3893792212e409ca03c258b97ff3b7e624948980498222d2110a85a4e9034024490211fa15fc085e6d3132037aa84155332732d224a92c07600219aaaadb1280133165124252aa95ecec77cabc4a5a5cb8f14e91f95bddb8c13f324184beecc516473739df25bce5fac0e2988b5c2824fdc6306d486781180519806b0c9c652d3ac453e3df1375ba2eee562edb813ad43b0a2000bf63b9ee48f757c2ae8e4614283828782f69b7b40ce092447ce5bb5bf1820eeebca440c8c53d8cc92f559d2263cc6357e1d07345b477619e43b7e485149c03349082376c6e3d243998a3cf32d0c03566b43a43c702ae9303e80a6180c368a2ac923e39db9d1ad01275e32673167163bb01509998b584e8cf99e6e74576554df2a4ad24c6e73f385f1178afc350a7f055ed9a";

struct ZSample {
    const char* name;
    const char* hex;
    Bytes plain;
};

static std::vector<ZSample> zSamples() {
    return {{"text, level 1", kZText1, genText(7, 1200)},          {"text, level 3 + checksum", kZText3, genText(7, 1200)},
            {"text, level 19", kZText19, genText(99, 2500)},        {"noise (raw block)", kZNoise, genNoise(5, 300)},
            {"a run (RLE)", kZRun, genRun()},                       {"skewed bytes (Huffman literals)", kZSkew, genSkew(11, 1500)},
            {"wide alphabet (FSE-coded Huffman weights)", kZWide, genWide(13, 1500)},
            {"sixteen symbols (direct 4-bit Huffman weights)", kZNibbles, genNibbles(21, 800)},
            {"periodic, three blocks", kZPeriodic, genPeriodic(300000)}};
}

static void testZstd() {
    AVER_INFO("=== Zstandard ===");
    const std::vector<ZSample> samples = zSamples();
    for (const ZSample& s : samples) {
        const Bytes z = fromHex(s.hex);
        Bytes out;
        std::string why;
        const bool ok = zstdDecompress(z.data(), z.size(), out, usize(1) << 20, &why);
        check(ok && out == s.plain, std::string("decodes ") + s.name + (ok ? "" : ": " + why));
    }

    // Hand-built frames: a raw block, an RLE block, two frames back to back, a skippable frame in between.
    const Bytes raw = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x05, 0x29, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};
    const Bytes rle = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x0A, 0x53, 0x00, 0x00, 'z'};
    Bytes out;
    std::string why;
    check(zstdDecompress(raw.data(), raw.size(), out, 100, &why) && std::string(out.begin(), out.end()) == "hello", "a raw block");
    out.clear();
    check(zstdDecompress(rle.data(), rle.size(), out, 100, &why) && std::string(out.begin(), out.end()) == "zzzzzzzzzz", "an RLE block");
    Bytes both = raw;
    const Bytes skip = {0x50, 0x2A, 0x4D, 0x18, 0x03, 0x00, 0x00, 0x00, 1, 2, 3};
    both.insert(both.end(), skip.begin(), skip.end());
    both.insert(both.end(), rle.begin(), rle.end());
    out.clear();
    check(zstdDecompress(both.data(), both.size(), out, 100, &why) && std::string(out.begin(), out.end()) == "hellozzzzzzzzzz", "concatenated frames with a skippable frame between them");

    // The output cap: an RLE block asks for 10 bytes; with a cap of 9 it must stop.
    out.clear();
    check(!zstdDecompress(rle.data(), rle.size(), out, 9, &why) && why.find("limit") != std::string::npos, "the output cap stops an RLE block");
    out.clear();
    const Bytes z = fromHex(kZPeriodic);
    check(!zstdDecompress(z.data(), z.size(), out, 150000, &why) && out.size() <= 150000, "the output cap stops a multi-block frame, and nothing past it is written");

    // Things that are not valid.
    Bytes bad = raw;
    bad[0] = 0x27;
    out.clear();
    check(!zstdDecompress(bad.data(), bad.size(), out, 100, &why) && why.find("not a Zstandard frame") != std::string::npos, "a wrong magic number");
    Bytes dict = {0x28, 0xB5, 0x2F, 0xFD, 0x21, 0x07, 0x05, 0x29, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};   // dictionary id 7 (1 byte)
    out.clear();
    check(!zstdDecompress(dict.data(), dict.size(), out, 100, &why) && why.find("dictionary") != std::string::npos, "a dictionary frame is refused");
    Bytes reserved = raw;
    reserved[6] = 0x07;   // block type 3
    out.clear();
    check(!zstdDecompress(reserved.data(), reserved.size(), out, 100, &why), "a reserved block type");
    Bytes mismatch = raw;
    mismatch[5] = 0x06;   // the frame says 6 bytes, the block holds 5
    out.clear();
    check(!zstdDecompress(mismatch.data(), mismatch.size(), out, 100, &why), "a content size that does not match");
    out.clear();
    check(!zstdDecompress(raw.data(), raw.size() - 1, out, 100, &why), "a raw block cut short");
    out.clear();
    check(!zstdDecompress(raw.data(), 0, out, 100, &why), "no frame at all");

    // Hostile sweep: every prefix, and every byte flipped, of every sample. A cut one-frame stream is never complete; a flipped one may decode to
    // anything, but it must not crash, hang or pass the cap.
    usize prefixes = 0, flips = 0, complete = 0, overCap = 0;
    for (const ZSample& s : samples) {
        const Bytes full = fromHex(s.hex);
        for (usize n = 0; n < full.size(); ++n) {
            Bytes o;
            std::string w;
            if (zstdDecompress(full.data(), n, o, usize(1) << 20, &w)) ++complete;
            ++prefixes;
        }
        for (usize n = 0; n < full.size(); ++n) {
            Bytes mut = full;
            mut[n] ^= 0x5A;
            Bytes o;
            std::string w;
            zstdDecompress(mut.data(), mut.size(), o, usize(1) << 20, &w);
            if (o.size() > (usize(1) << 20)) ++overCap;
            ++flips;
        }
    }
    check(complete == 0 && overCap == 0, "no cut sample decodes as complete and no mutation passes the cap (" + std::to_string(prefixes) + " prefixes, " + std::to_string(flips) + " flips)");
    Lcg rnd(2024);
    for (int i = 0; i < 400; ++i) {
        Bytes junk(8 + rnd.next() % 200);
        for (u8& b : junk) b = u8(rnd.next() >> 8);
        junk[0] = 0x28; junk[1] = 0xB5; junk[2] = 0x2F; junk[3] = 0xFD;   // a real magic, then noise
        Bytes o;
        std::string w;
        zstdDecompress(junk.data(), junk.size(), o, usize(1) << 20, &w);
    }
    check(true, "400 magic-prefixed noise buffers decode or fail without a crash");
}

// ---- a resource with every Variant type -------------------------------------------------------------------------------------

struct ResFixture {
    FileDef d;
    Bytes bytes;
};

static ResFixture buildResource(bool be, bool real64) {
    ResFixture rf;
    FileDef& d = rf.d;
    d.f.be = be;
    d.f.real64 = real64;
    d.cls = "Foo";
    d.ext.push_back({"Texture2D", "res://tex.png", 0x3d88e35046e8e0c3ULL});
    d.ext.push_back({"Material", "res://mat.tres", 0});
    FileDef::Res sub;
    sub.path = "local://BoxMesh_a1";
    sub.type = "BoxMesh";
    d.res.push_back(sub);
    d.prop(d.res.back(), "size", d.f.reals(kVector3, {1, 2, 3}));
    FileDef::Res mainRes;
    mainRes.path = "res://t.res";
    mainRes.type = "Foo";
    d.res.push_back(mainRes);
    FileDef::Res& m = d.res.back();
    const Fmt& f = d.f;
    d.prop(m, "b", f.boolean(true));
    d.prop(m, "i", f.integer(-5));
    d.prop(m, "i64", f.int64(1ll << 40));
    d.prop(m, "f", f.flt(1.5f));
    d.prop(m, "d", f.dbl(2.25));
    d.prop(m, "s", f.text(std::string("h") + "\xC3\xA9" + "llo"));
    d.prop(m, "sn", f.text("abc", kStringName));
    d.prop(m, "v2", f.reals(kVector2, {1, 2}));
    d.prop(m, "v2i", f.ints(kVector2i, {3, -4}));
    d.prop(m, "v3", f.reals(kVector3, {0.5, 1.5, -2.5}));
    d.prop(m, "v3i", f.ints(kVector3i, {7, 8, 9}));
    d.prop(m, "v4", f.reals(kVector4, {1, 2, 3, 4}));
    d.prop(m, "v4i", f.ints(kVector4i, {5, 6, 7, 8}));
    d.prop(m, "r2", f.reals(kRect2, {1, 2, 3, 4}));
    d.prop(m, "r2i", f.ints(kRect2i, {1, 2, 3, 4}));
    d.prop(m, "pl", f.reals(kPlane, {0, 1, 0, -2}));
    d.prop(m, "q", f.reals(kQuaternion, {0, 0, 0.5, 1}));
    d.prop(m, "aabb", f.reals(kAabb, {-1, -2, -3, 2, 4, 6}));
    d.prop(m, "bas", f.reals(kBasis, {1, 2, 3, 4, 5, 6, 7, 8, 9}));
    d.prop(m, "t2", f.reals(kTransform2D, {1, 0, 0, 1, 5, 6}));
    d.prop(m, "t3", f.reals(kTransform3D, {1, 0, 0, 0, 1, 0, 0, 0, 1, 10, 20, 30}));
    d.prop(m, "proj", f.reals(52, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}));
    d.prop(m, "col", f.color(0.25f, 0.5f, 0.75f, 1.0f));
    d.prop(m, "np", f.nodePath({d.sidx("root"), d.sidx("A")}, {d.sidx("position")}, true));
    d.prop(m, "np2", f.nodePath({d.sidx(".."), d.sidx("B")}, {}, false));
    d.prop(m, "rid", f.rid(7));
    d.prop(m, "dict", f.dict({{f.text("a"), f.integer(1)}, {f.text("b"), f.array({f.text("x"), f.nil()})}}));
    d.prop(m, "arr", f.array({f.nil(), f.integer(2), f.reals(kVector2, {3, 4})}));
    d.prop(m, "pb", f.packedBytes({1, 2, 3, 4, 5}));
    d.prop(m, "pb0", f.packedBytes({}));
    d.prop(m, "pb4", f.packedBytes({9, 8, 7, 6}));
    d.prop(m, "pi32", f.packedI32({1, -2, 3}));
    d.prop(m, "pi64", f.packedI64({1ll << 35, -1}));
    d.prop(m, "pf32", f.packedF32({0.5f, 1.5f}));
    d.prop(m, "pf64", f.packedF64({0.125, 2.5}));
    d.prop(m, "ps", f.packedStrings({"one", "", "three"}));
    d.prop(m, "pv2", f.packedReals(kPackedVector2, {1, 2, 3, 4}));
    d.prop(m, "pv3", f.packedReals(kPackedVector3, {1, 2, 3, 4, 5, 6}));
    d.prop(m, "pv4", f.packedReals(kPackedVector4, {1, 2, 3, 4}));
    d.prop(m, "pc", f.packedColors({0, 0.5f, 1, 1}));
    d.prop(m, "none", f.objectNone());
    d.prop(m, "ext", f.extRef(1));
    d.prop(m, "sub", f.subRef(0));
    d.prop(m, "script", f.nil());
    d.prop(m, "callable", f.tag(kCallable));
    rf.bytes = d.build();
    return rf;
}

static void testResource() {
    AVER_INFO("=== a resource with every Variant type ===");
    const ResFixture rf = buildResource(false, false);
    GodotResource r;
    GodotDiagnostics d;
    check(parseRes(rf.bytes, r, d) && d.items.empty(), "it parses with no diagnostics: " + d.format());
    check(r.header.kind == "gd_resource" && r.header.type == "Foo" && r.header.format == 3 && r.file == "res://t.res", "header: kind, main class, format 3, file name");
    check(r.header.uid == godotUidText(0x6f607bd40182c27fULL) && r.header.uid == "uid://dmlvwah3ypol0", "the header uid is spelled the way Godot spells it");
    check(godotUidText(0x3d88e35046e8e0c3ULL) == "uid://b4f5b2j76nf2b" && godotUidText(0).empty() && godotUidText(u64(1) << 63).empty(), "uid text: another known pair, and the invalid ids are empty");
    check(r.extResources.size() == 2 && r.extResources[0].id == "1" && r.extResources[0].type == "Texture2D" && r.extResources[0].path == "res://tex.png" &&
              r.extResources[0].uid == "uid://b4f5b2j76nf2b" && r.extResources[1].uid.empty(),
          "external resources: ids 1 and 2, type, path, uid");
    check(r.subResources.size() == 1 && r.subResources[0].id == "BoxMesh_a1" && r.subResources[0].type == "BoxMesh" && numsAre(r.subResources[0].prop("size"), {1, 2, 3}),
          "the sub-resource is named by its local:// id and keeps its properties");

    auto P = [&](const char* name) { return r.prop(name); };
    check(P("b") && P("b")->type == GodotVType::Bool && P("b")->b, "Bool");
    check(P("i") && P("i")->type == GodotVType::Int && P("i")->i == -5, "Int (32-bit, negative)");
    check(P("i64") && P("i64")->type == GodotVType::Int && P("i64")->i == (1ll << 40), "Int64");
    check(P("f") && P("f")->type == GodotVType::Float && near(P("f")->f, 1.5) && P("d") && near(P("d")->f, 2.25), "Float and Double");
    check(P("s") && P("s")->type == GodotVType::String && P("s")->s == std::string("h") + "\xC3\xA9" + "llo", "String keeps its UTF-8 bytes and drops the NUL");
    check(P("sn") && P("sn")->type == GodotVType::StringName && P("sn")->s == "abc", "StringName");
    check(P("v2") && P("v2")->type == GodotVType::Vector2 && numsAre(P("v2"), {1, 2}), "Vector2");
    check(P("v2i") && P("v2i")->type == GodotVType::Vector2i && numsAre(P("v2i"), {3, -4}), "Vector2i");
    check(P("v3") && P("v3")->type == GodotVType::Vector3 && numsAre(P("v3"), {0.5, 1.5, -2.5}), "Vector3");
    check(P("v3i") && numsAre(P("v3i"), {7, 8, 9}) && P("v4") && P("v4")->type == GodotVType::Vector4 && numsAre(P("v4"), {1, 2, 3, 4}) && P("v4i") && numsAre(P("v4i"), {5, 6, 7, 8}), "Vector3i, Vector4, Vector4i");
    check(P("r2") && P("r2")->type == GodotVType::Rect2 && numsAre(P("r2"), {1, 2, 3, 4}) && P("r2i") && P("r2i")->type == GodotVType::Rect2i && numsAre(P("r2i"), {1, 2, 3, 4}), "Rect2 and Rect2i");
    check(P("pl") && P("pl")->type == GodotVType::Plane && numsAre(P("pl"), {0, 1, 0, -2}) && P("q") && P("q")->type == GodotVType::Quaternion && numsAre(P("q"), {0, 0, 0.5, 1}), "Plane (normal, d) and Quaternion (x y z w)");
    check(P("aabb") && P("aabb")->type == GodotVType::AABB && numsAre(P("aabb"), {-1, -2, -3, 2, 4, 6}), "AABB (position, size)");
    check(P("bas") && P("bas")->type == GodotVType::Basis && numsAre(P("bas"), {1, 2, 3, 4, 5, 6, 7, 8, 9}), "Basis: nine numbers row by row, as the text form lists them");
    check(P("t2") && P("t2")->type == GodotVType::Transform2D && numsAre(P("t2"), {1, 0, 0, 1, 5, 6}), "Transform2D: x column, y column, origin");
    check(P("t3") && P("t3")->type == GodotVType::Transform3D && numsAre(P("t3"), {1, 0, 0, 0, 1, 0, 0, 0, 1, 10, 20, 30}), "Transform3D: nine basis numbers then the origin");
    check(P("proj") && P("proj")->type == GodotVType::Projection && P("proj")->nums.size() == 16, "Projection");
    check(P("col") && P("col")->type == GodotVType::Color && numsAre(P("col"), {0.25, 0.5, 0.75, 1}), "Color");
    check(P("np") && P("np")->type == GodotVType::NodePath && P("np")->s == "/root/A:position", "NodePath: absolute, names, a property subname");
    check(P("np2") && P("np2")->s == "../B", "NodePath: relative");
    check(P("rid") && P("rid")->type == GodotVType::Rid && P("rid")->i == 7, "RID");
    const GodotVariant* dict = P("dict");
    check(dict && dict->type == GodotVType::Dictionary && dict->count() == 2 && dict->find("a") && dict->find("a")->i == 1 && dict->find("b") && dict->find("b")->items.size() == 2 &&
              dict->find("b")->items[0].s == "x" && dict->find("b")->items[1].isNil(),
          "Dictionary with a nested Array");
    check(P("arr") && P("arr")->type == GodotVType::Array && P("arr")->items.size() == 3 && P("arr")->items[0].isNil() && P("arr")->items[1].i == 2 && numsAre(&P("arr")->items[2], {3, 4}), "Array");
    check(P("pb") && P("pb")->type == GodotVType::PackedByteArray && P("pb")->bytes == Bytes({1, 2, 3, 4, 5}), "PackedByteArray (length 5: 3 padding bytes skipped, the next property still reads)");
    check(P("pb0") && P("pb0")->bytes.empty() && P("pb4") && P("pb4")->bytes == Bytes({9, 8, 7, 6}), "PackedByteArray of length 0 and 4 (no padding)");
    check(P("pi32") && P("pi32")->type == GodotVType::PackedInt32Array && numsAre(P("pi32"), {1, -2, 3}), "PackedInt32Array");
    check(P("pi64") && P("pi64")->type == GodotVType::PackedInt64Array && numsAre(P("pi64"), {34359738368.0, -1}), "PackedInt64Array");
    check(P("pf32") && numsAre(P("pf32"), {0.5, 1.5}) && P("pf64") && P("pf64")->type == GodotVType::PackedFloat64Array && numsAre(P("pf64"), {0.125, 2.5}), "PackedFloat32Array and PackedFloat64Array");
    check(P("ps") && P("ps")->type == GodotVType::PackedStringArray && P("ps")->strs == std::vector<std::string>({"one", "", "three"}), "PackedStringArray");
    check(P("pv2") && P("pv2")->type == GodotVType::PackedVector2Array && numsAre(P("pv2"), {1, 2, 3, 4}) && P("pv3") && P("pv3")->type == GodotVType::PackedVector3Array &&
              numsAre(P("pv3"), {1, 2, 3, 4, 5, 6}) && P("pv4") && P("pv4")->type == GodotVType::PackedVector4Array && numsAre(P("pv4"), {1, 2, 3, 4}),
          "PackedVector2/3/4Array, flat");
    check(P("pc") && P("pc")->type == GodotVType::PackedColorArray && numsAre(P("pc"), {0, 0.5, 1, 1}), "PackedColorArray");
    check(P("none") && P("none")->isNil(), "an empty Object is Nil");
    check(P("ext") && P("ext")->type == GodotVType::ExtResource && P("ext")->s == "2" && r.findExt("2") && r.findExt("2")->path == "res://mat.tres", "an external-resource Object is ExtResource(id) and the id resolves");
    check(P("sub") && P("sub")->type == GodotVType::SubResource && P("sub")->s == "BoxMesh_a1" && r.findSub("BoxMesh_a1"), "an internal-resource Object is SubResource(id) and the id resolves");
    check(!P("script"), "a nil script property is dropped, as the text form leaves it out");
    check(P("callable") && P("callable")->type == GodotVType::Callable, "Callable (no payload)");

    // Endianness and real width.
    const ResFixture be = buildResource(true, false);
    GodotResource rb;
    GodotDiagnostics db;
    check(parseRes(be.bytes, rb, db) && db.items.empty(), "a big-endian file parses: " + db.format());
    check(rb.prop("i") && rb.prop("i")->i == -5 && rb.prop("i64") && rb.prop("i64")->i == (1ll << 40) && numsAre(rb.prop("v3"), {0.5, 1.5, -2.5}) && rb.prop("s") && rb.prop("s")->s == std::string("h") + "\xC3\xA9" + "llo" &&
              rb.prop("np") && rb.prop("np")->s == "/root/A:position" && numsAre(rb.prop("pi32"), {1, -2, 3}) && rb.prop("pb") && rb.prop("pb")->bytes == Bytes({1, 2, 3, 4, 5}),
          "big-endian: integers, floats, strings, node paths and arrays read the same");
    check(rb.header.uid == r.header.uid && rb.extResources[0].uid == r.extResources[0].uid, "big-endian: the uids are the same");
    const ResFixture r64 = buildResource(false, true);
    GodotResource rd;
    GodotDiagnostics dd;
    check(parseRes(r64.bytes, rd, dd) && dd.items.empty(), "a double-precision file parses: " + dd.format());
    check(numsAre(rd.prop("v3"), {0.5, 1.5, -2.5}) && numsAre(rd.prop("t3"), {1, 0, 0, 0, 1, 0, 0, 0, 1, 10, 20, 30}) && numsAre(rd.prop("pv3"), {1, 2, 3, 4, 5, 6}) && numsAre(rd.prop("col"), {0.25, 0.5, 0.75, 1}),
          "double-precision: vectors, transforms and packed vectors use 8-byte reals, colours stay 4-byte");
    const ResFixture both = buildResource(true, true);
    GodotResource rbb;
    GodotDiagnostics dbb;
    check(parseRes(both.bytes, rbb, dbb) && dbb.items.empty() && numsAre(rbb.prop("v3"), {0.5, 1.5, -2.5}) && rbb.prop("i") && rbb.prop("i")->i == -5, "big-endian and double-precision together");

    // The header on its own.
    GodotBinaryHeader h;
    std::string why;
    check(readGodotBinaryHeader(rf.bytes.data(), rf.bytes.size(), true, h, &why) && h.className == "Foo" && h.majorVersion == 4 && h.minorVersion == 7 && h.formatVersion == 6 && !h.compressed && !h.bigEndian &&
              h.uid == "uid://dmlvwah3ypol0",
          "readGodotBinaryHeader: class, engine version, format version, uid");
    bool more = false;
    check(readGodotBinaryHeader(rf.bytes.data(), 20, false, h, &why, &more) == false, "a header cut inside its fields fails");
}

// ---- a PackedScene ----------------------------------------------------------------------------------------------------------

struct SceneFixture {
    FileDef d;
    SceneDef s;
    Bytes bytes;
};

static SceneFixture buildScene(bool connLayoutB, bool inherited) {
    SceneFixture sf;
    FileDef& d = sf.d;
    SceneDef& s = sf.s;
    d.ext.push_back({"PackedScene", "res://inst.tscn", 0x3d88e35046e8e0c3ULL});
    d.ext.push_back({"PackedScene", "res://base.tscn", 0});
    FileDef::Res sub;
    sub.path = "local://BoxMesh_a1";
    sub.type = "BoxMesh";
    d.res.push_back(sub);
    d.prop(d.res.back(), "size", d.f.reals(kVector3, {1, 2, 3}));

    s.names = {"Root", "Node3D", "Child", "MeshInstance3D", "transform", "mesh", "Inst", "Label3D", "text", "grp", "pressed", "_on_pressed", "visible", "target", "Part", "Leaf", "Deep"};
    const Fmt& f = d.f;
    s.variants = {f.extRef(0),                                                                    // 0 the instanced scene
                  f.reals(kTransform3D, {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 2, 3}),                    // 1
                  f.subRef(0),                                                                    // 2 the box mesh
                  f.nodePath({d.sidx(".."), d.sidx("Inst")}, {}, false),                          // 3
                  f.boolean(false),                                                               // 4
                  f.text("hello"),                                                                // 5
                  f.reals(kVector3, {0, 1, 0}),                                                   // 6 a bind
                  f.extRef(1)};                                                                   // 7 the base scene
    s.nodePaths = {f.nodePath({d.sidx("Inst")}, {}, false), f.nodePath({d.sidx("Inst"), d.sidx("Sub")}, {}, false)};
    s.editable = {f.nodePath({d.sidx("Inst")}, {}, false)};

    const i32 kInst = 0x7FFFFFFF, kPath = 1 << 30;
    auto node = [&](i32 parent, i32 owner, i32 type, i32 name, i32 instance, std::vector<std::pair<i32, i32>> props, std::vector<i32> groups) {
        for (i32 v : {parent, owner, type, name, instance, i32(props.size())}) s.nodes.push_back(v);
        for (const auto& p : props) { s.nodes.push_back(p.first); s.nodes.push_back(p.second); }
        s.nodes.push_back(i32(groups.size()));
        for (i32 g : groups) s.nodes.push_back(g);
        ++s.nodeCount;
    };
    node(-1, -1, 1, 0, -1, {{4, 1}}, {9});                                         // 0 Root: Node3D, in group "grp"
    node(0, 0, 3, 2, -1, {{4, 1}, {5, 2}, {13 | kPath, 3}}, {});                   // 1 Child: MeshInstance3D; target is a node-path property
    node(0, 0, kInst, 6, 0, {{4, 1}}, {});                                         // 2 Inst: an instance of inst.tscn
    node(0 | kPath, 0, 7, 16, -1, {{8, 5}}, {});                                   // 3 Deep: a Label3D added below the instance (parent path "Inst")
    node(1 | kPath, 0, kInst, 14, -1, {{12, 4}}, {});                              // 4 Part: edits a node inside the instance (parent path "Inst/Sub")
    node(1, 0, 1, 15, -1, {}, {});                                                 // 5 Leaf below Child
    for (i32 id : {100, 101, 102, 103, 104, 105}) s.nodeIds.push_back(id);
    s.conns = {1, 0, 10, 11, 1, 0, 0};                                              // Child -> Root pressed -> _on_pressed, flags 1, no unbinds, no binds
    if (!connLayoutB) {
        for (i32 v : {5, 0, 10, 11, 3, 2, 1, 6}) s.conns.push_back(v);               // Leaf -> Root: flags 3, unbinds 2, one bind (variant 6)
    } else {
        for (i32 v : {5, 0, 10, 11, 3, 1, 6, 2}) s.conns.push_back(v);               // the same with the bind list before the unbind count
    }
    s.connCount = 2;
    if (inherited) s.baseScene = 7;
    addMainScene(d, s);
    d.uid = 0x6f607bd40182c27fULL;
    sf.bytes = d.build();
    return sf;
}

static void checkScene(const GodotScene& s, bool inherited, const std::string& tag) {
    check(s.header.kind == "gd_scene" && s.header.format == 3 && s.header.uid == "uid://dmlvwah3ypol0" && s.file == "res://t.scn", tag + ": header");
    check(s.extResources.size() == 2 && s.extResources[0].id == "1" && s.extResources[0].path == "res://inst.tscn" && s.subResources.size() == 1 && s.subResources[0].id == "BoxMesh_a1",
          tag + ": external and sub resources (the PackedScene itself is not a sub-resource)");
    check(s.nodes.size() == 6, tag + ": six nodes");
    if (s.nodes.size() != 6) return;
    const GodotNode& root = s.nodes[0];
    check(root.isRoot && root.name == "Root" && root.path == "." && root.parent == -1 && root.groups == std::vector<std::string>({"grp"}), tag + ": the root, its path and group");
    if (!inherited) check(root.type == "Node3D" && !root.isInstance(), tag + ": root class Node3D");
    else check(root.type.empty() && root.instance == "2", tag + ": an inherited scene's root is an instance of its base scene (ExtResource 2) and has no class of its own");
    check(root.prop("transform") && root.prop("transform")->type == GodotVType::Transform3D && numsAre(root.prop("transform"), {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 2, 3}), tag + ": the root's transform");
    const GodotNode& child = s.nodes[1];
    check(child.name == "Child" && child.type == "MeshInstance3D" && child.path == "Child" && child.parent == 0 && child.parentPath == ".", tag + ": a child of the root");
    check(child.prop("mesh") && child.prop("mesh")->type == GodotVType::SubResource && child.prop("mesh")->s == "BoxMesh_a1", tag + ": a SubResource property");
    check(child.prop("target") && child.prop("target")->type == GodotVType::NodePath && child.prop("target")->s == "../Inst" && child.nodePaths == std::vector<std::string>({"target"}),
          tag + ": a node-path property keeps its path and is listed in nodePaths");
    const GodotNode& inst = s.nodes[2];
    check(inst.name == "Inst" && inst.type.empty() && inst.instance == "1" && inst.isInstance() && !inst.isOverride && inst.parent == 0, tag + ": an instance of ExtResource 1");
    const GodotNode& deep = s.nodes[3];
    check(deep.name == "Deep" && deep.type == "Label3D" && deep.path == "Inst/Deep" && deep.parent == 2 && !deep.unresolvedParent && deep.prop("text") && deep.prop("text")->s == "hello",
          tag + ": a node added below an instance by parent path");
    const GodotNode& part = s.nodes[4];
    check(part.name == "Part" && part.type.empty() && part.isOverride && part.path == "Inst/Sub/Part" && part.unresolvedParent && part.anchor == 2 && part.prop("visible") && !part.prop("visible")->b,
          tag + ": an override of a node inside an instance is unresolved and anchored at the instance");
    const GodotNode& leaf = s.nodes[5];
    check(leaf.name == "Leaf" && leaf.path == "Child/Leaf" && leaf.parent == 1 && leaf.type == "Node3D", tag + ": a grandchild's path");
    check(root.uniqueId == "100" && leaf.uniqueId == "105" && root.index == -1, tag + ": unique ids come from node_ids; the sibling index is not recovered");
    check(s.connections.size() == 2, tag + ": two connections");
    if (s.connections.size() == 2) {
        const GodotConnection& c0 = s.connections[0];
        const GodotConnection& c1 = s.connections[1];
        check(c0.from == "Child" && c0.to == "." && c0.signal == "pressed" && c0.method == "_on_pressed" && c0.flags == 1 && c0.unbinds == 0 && c0.binds.empty(), tag + ": a plain connection");
        check(c1.from == "Child/Leaf" && c1.to == "." && c1.flags == 3 && c1.unbinds == 2 && c1.binds.size() == 1 && numsAre(&c1.binds[0], {0, 1, 0}), tag + ": a connection with a bind and unbinds");
    }
    check(s.editablePaths == std::vector<std::string>({"Inst"}), tag + ": editable instances");
}

static void testScene() {
    AVER_INFO("=== a PackedScene ===");
    {
        const SceneFixture sf = buildScene(false, false);
        GodotScene s;
        GodotDiagnostics d;
        check(parseScn(sf.bytes, s, d) && d.items.empty(), "a scene parses with no diagnostics: " + d.format());
        checkScene(s, false, "scene");
        GodotResource asRes;
        GodotDiagnostics dr;
        check(parseRes(sf.bytes, asRes, dr) && asRes.header.type == "PackedScene" && asRes.prop("_bundled") && asRes.prop("_bundled")->type == GodotVType::Dictionary,
              "the same file read as a plain resource is a PackedScene with its _bundled dictionary");
    }
    {
        const SceneFixture sf = buildScene(true, false);
        GodotScene s;
        GodotDiagnostics d;
        check(parseScn(sf.bytes, s, d) && d.items.empty(), "connections written with the bind list before the unbind count: " + d.format());
        check(s.connections.size() == 2 && s.connections[1].binds.size() == 1 && s.connections[1].unbinds == 2 && s.connections[1].flags == 3, "the other connection layout is recognised");
    }
    {
        const SceneFixture sf = buildScene(false, true);
        GodotScene s;
        GodotDiagnostics d;
        check(parseScn(sf.bytes, s, d), "an inherited scene parses: " + d.format());
        checkScene(s, true, "inherited");
    }
    {   // big-endian: the node table, the names and the dictionary all change byte order
        FileDef d;
        d.f.be = true;
        SceneDef s;
        s.names = {"Root", "Node3D", "transform"};
        s.variants = {d.f.reals(kVector3, {1, 2, 3})};
        s.nodes = {-1, -1, 1, 0, -1, 1, 2, 0, 0};
        s.nodeCount = 1;
        addMainScene(d, s);
        GodotScene out;
        GodotDiagnostics dd;
        check(parseScn(d.build(), out, dd) && out.nodes.size() == 1 && out.nodes[0].name == "Root" && out.nodes[0].type == "Node3D" && numsAre(out.nodes[0].prop("transform"), {1, 2, 3}), "a big-endian scene");
    }

    // Lies in the node table. The table is: node 0 at ints 0..9, node 1 at 10..22, node 2 at 23..31.
    auto sceneWith = [&](const std::function<void(SceneDef&)>& mutate) {
        SceneFixture sf = buildScene(false, false);
        mutate(sf.s);
        FileDef d2;
        d2.ext = sf.d.ext;
        d2.res.clear();
        d2.strings = sf.d.strings;
        FileDef::Res sub = sf.d.res[0];
        d2.res.push_back(sub);
        addMainScene(d2, sf.s);
        return d2.build();
    };
    auto rejects = [&](const char* what, const Bytes& b, const char* needle) {
        GodotScene s;
        GodotDiagnostics d;
        const bool ok = parseScn(b, s, d);
        check(!ok && firstError(d).find(needle) != std::string::npos, std::string(what) + ": " + firstError(d));
    };
    rejects("node_count larger than the table", sceneWith([](SceneDef& s) { s.nodeCount = 1000; }), "node_count");
    rejects("a name index past the table", sceneWith([](SceneDef& s) { s.nodes[3] = 500; }), "name table");
    rejects("a parent that is a later node", sceneWith([](SceneDef& s) { s.nodes[10] = 4; }), "parent");
    rejects("a property value past the variants", sceneWith([](SceneDef& s) { s.nodes[7] = 99; }), "variant table");
    rejects("a negative property count", sceneWith([](SceneDef& s) { s.nodes[5] = -1; }), "properties");
    rejects("an instance past the variants", sceneWith([](SceneDef& s) { s.nodes[27] = 99; }), "variant table");
    rejects("a parent path past node_paths", sceneWith([](SceneDef& s) { s.nodes[32] = (1 << 30) | 9; }), "node_paths");
    {
        FileDef d;
        d.cls = "Foo";
        FileDef::Res m;
        m.path = "res://t.res";
        m.type = "Foo";
        d.res.push_back(m);
        const Bytes b = d.build();
        rejects("a resource that is not a PackedScene read as a scene", b, "not a PackedScene");
    }
}

// ---- RSCC -----------------------------------------------------------------------------------------------------------------------

static void testCompressed() {
    AVER_INFO("=== RSCC: the compressed container ===");
    const SceneFixture sf = buildScene(false, false);
    const Bytes stream = sf.d.build(false);
    for (u32 block : {4096u, 100u, 64u}) {
        const Bytes rscc = wrapRscc(stream, block);
        GodotScene s;
        GodotDiagnostics d;
        check(parseScn(rscc, s, d) && d.items.empty(), "a compressed scene (blocks of " + std::to_string(block) + " bytes) parses: " + d.format());
        checkScene(s, false, "compressed/" + std::to_string(block));
        GodotBinaryHeader h;
        std::string why;
        bool more = false;
        check(readGodotBinaryHeader(rscc.data(), rscc.size(), true, h, &why) && h.compressed && h.className == "PackedScene" && h.uid == "uid://dmlvwah3ypol0", "the header is read through the first block");
        check(readGodotBinaryHeader(rscc.data(), std::min<usize>(rscc.size(), 40), false, h, &why, &more) == false && more, "a prefix that cuts the first block asks for more of the file");
    }
    {   // a stream that is an exact multiple of the block size ends with an empty block
        Bytes padded = stream;
        const u32 block = 64;
        while (padded.size() % block != 0) padded.push_back(0);
        const Bytes rscc = wrapRscc(padded, block);
        GodotScene s;
        GodotDiagnostics d;
        // trailing zero bytes make the end marker fall short of the end: a warning, not an error
        check(parseScn(rscc, s, d) && s.nodes.size() == 6, "a stream that fills its last block exactly still reads");
    }
    const Bytes rscc = wrapRscc(stream, 4096);
    auto patch32 = [](Bytes b, usize at, u32 v) {
        for (int i = 0; i < 4; ++i) b[at + usize(i)] = u8(v >> (8 * i));
        return b;
    };
    auto fails = [&](const char* what, const Bytes& b, const char* needle) {
        GodotScene s;
        GodotDiagnostics d;
        const bool ok = parseScn(b, s, d);
        check(!ok && firstError(d).find(needle) != std::string::npos, std::string(what) + ": " + firstError(d));
    };
    fails("compression mode 1 (Deflate)", patch32(rscc, 4, 1), "compression mode 1");
    fails("compression mode 0 (FastLZ)", patch32(rscc, 4, 0), "compression mode 0");
    fails("a zero block size", patch32(rscc, 8, 0), "block size");
    fails("a huge block size", patch32(rscc, 8, 0xFFFFFFFFu), "block size");
    fails("a total beyond the limit", patch32(rscc, 12, 0x7FFFFFFFu), "limit");
    fails("a block table that runs past the file", patch32(rscc, 12, 0x00FFFFFFu), "block table");
    fails("a compressed size past the file", patch32(rscc, 16, 0x00FFFFFFu), "runs past");
    fails("a total that disagrees with the blocks", patch32(rscc, 12, u32(stream.size()) + 5), "");
    Bytes badFrame = rscc;
    badFrame[16 + 4 + 1] ^= 0xFF;   // the first block's frame magic
    fails("a block that is not a Zstandard frame", badFrame, "block 0");
    fails("only the magic", Bytes{'R', 'S', 'C', 'C'}, "");

    // A really compressed block: this scene's stream, compressed at level 9 by a reference compressor and wrapped as Godot wraps it.
    // If the fixture builder changes, regenerate the hex from `GodotBinaryTest --dump-stream <file>`.
    {
        const Bytes frame = fromHex(kZScene);
        Enc e;
        e.raw("RSCC", 4);
        e.u32v(2);
        e.u32v(4096);
        e.u32v(u32(stream.size()));
        e.u32v(u32(frame.size()));
        e.raw(frame.data(), frame.size());
        e.raw("GCMP", 4);
        Bytes plain;
        std::string zwhy;
        check(zstdDecompress(frame.data(), frame.size(), plain, usize(1) << 20, &zwhy) && plain == stream, "the pasted scene frame decodes to the stream the builder makes (" + zwhy + ")");
        GodotScene s;
        GodotDiagnostics d;
        check(parseScn(e.b, s, d) && d.items.empty(), "a scene compressed for real parses: " + d.format());
        checkScene(s, false, "zstd");
    }
}

// ---- hostile input ---------------------------------------------------------------------------------------------------------

static void sweep(const char* what, const Bytes& file, bool scene, bool footerMatters) {
    usize outcomes[2] = {0, 0};
    usize wrongOk = 0;
    for (usize n = 0; n < file.size(); ++n) {
        const Bytes cut(file.begin(), file.begin() + isize(n));
        GodotDiagnostics d;
        bool ok;
        if (scene) { GodotScene s; ok = parseScn(cut, s, d); }
        else { GodotResource r; ok = parseRes(cut, r, d); }
        ++outcomes[ok ? 1 : 0];
        if (ok && footerMatters && n + 4 < file.size()) ++wrongOk;
    }
    check(wrongOk == 0, std::string(what) + ": no prefix that cuts into the data parses as a whole (" + std::to_string(file.size()) + " prefixes tried)");
    usize flipOk = 0;
    for (usize n = 0; n < file.size(); ++n) {
        for (u8 x : {u8(0xFF), u8(0x01), u8(0x80)}) {
            Bytes mut = file;
            mut[n] ^= x;
            GodotDiagnostics d;
            if (scene) { GodotScene s; flipOk += parseScn(mut, s, d) ? 1 : 0; }
            else { GodotResource r; flipOk += parseRes(mut, r, d) ? 1 : 0; }
        }
    }
    check(true, std::string(what) + ": " + std::to_string(file.size() * 3) + " single-byte mutations read or fail without a crash (" + std::to_string(flipOk) + " still parse)");
}

static void testHostile() {
    AVER_INFO("=== hostile input ===");
    const ResFixture rf = buildResource(false, false);
    sweep("resource", rf.bytes, false, true);
    sweep("big-endian resource", buildResource(true, false).bytes, false, true);
    const SceneFixture sf = buildScene(false, false);
    sweep("scene", sf.bytes, true, true);
    sweep("compressed scene", wrapRscc(sf.d.build(false), 128), true, false);

    auto rejects = [&](const char* what, const Bytes& b, const char* needle) {
        GodotResource r;
        GodotDiagnostics d;
        const bool ok = parseRes(b, r, d);
        const std::string e = firstError(d);
        check(!ok && e.find(needle) != std::string::npos, std::string(what) + ": " + e);
    };
    auto withHeader = [&](const std::function<void(FileDef&)>& mutate) {
        FileDef d;
        d.cls = "Foo";
        FileDef::Res m;
        m.path = "res://t.res";
        m.type = "Foo";
        d.res.push_back(m);
        d.prop(d.res.back(), "x", d.f.integer(1));
        mutate(d);
        return d.build();
    };
    rejects("empty input", Bytes{}, "RSRC");
    rejects("neither RSRC nor RSCC", Bytes{'G', 'D', 'S', 'C', 0, 0, 0, 0}, "RSRC");
    rejects("a Godot 3 binary", withHeader([](FileDef& d) { d.major = 3; d.format = 1; }), "Godot 3");
    rejects("a pre-Godot-4 format version", withHeader([](FileDef& d) { d.format = 2; }), "format version 2");
    {
        Bytes b = withHeader([](FileDef&) {});
        b[4] = 7;   // the endian flag
        rejects("a bad endian flag", b, "endian");
    }
    {
        FileDef d;
        d.cls = "Foo";
        d.format = 9;
        FileDef::Res m;
        m.path = "res://t.res";
        m.type = "Foo";
        d.res.push_back(m);
        d.prop(d.res.back(), "x", d.f.integer(1));
        GodotResource r;
        GodotDiagnostics dd;
        check(parseRes(d.build(), r, dd) && hasDiag(dd, GodotSeverity::Warning, "newer than the newest known") && r.prop("x") && r.prop("x")->i == 1, "a newer format version reads, with a warning");
    }
    // Lies about counts. Each rewrites a count to a huge value; none may allocate by it or hang.
    {
        FileDef d;
        d.cls = "Foo";
        d.strings = {"a", "b"};
        FileDef::Res m;
        m.path = "res://t.res";
        m.type = "Foo";
        d.res.push_back(m);
        const Bytes ok = d.build();
        // find the string-count word: it follows the 44 reserved bytes of the header
        usize at = 4 + 4 * 5 + (4 + 4) + 8 + 4 + 8 + 44;   // magic, 5 header words, class string "Foo\0", metadata offset, flags, uid, reserved
        Bytes b = ok;
        for (int i = 0; i < 4; ++i) b[at + usize(i)] = 0xFF;
        rejects("a string table of 4 billion entries", b, "string table");
        b = ok;
        at += 4 + (4 + 2) + (4 + 2);   // past the two strings
        for (int i = 0; i < 4; ++i) b[at + usize(i)] = 0xFF;
        rejects("4 billion external resources", b, "external resource table");
        b = ok;
        at += 4;
        for (int i = 0; i < 4; ++i) b[at + usize(i)] = 0xFF;
        rejects("4 billion internal resources", b, "internal resource table");
        b = ok;
        for (int i = 0; i < 4; ++i) b[at + usize(i)] = 0;
        rejects("no resources at all", b, "no resource");
    }
    {
        FileDef d;
        d.cls = "Foo";
        FileDef::Res m;
        m.path = "res://t.res";
        m.type = "Foo";
        d.res.push_back(m);
        d.prop(d.res.back(), "x", Bytes{});
        Bytes b = d.build();
        const usize x = b.size() - 4;
        // Replace the property's (empty) value with chosen bytes ahead of the footer.
        auto withValue = [&](const Bytes& v) {
            Bytes c(b.begin(), b.begin() + isize(x));
            c.insert(c.end(), v.begin(), v.end());
            c.insert(c.end(), {'R', 'S', 'R', 'C'});
            return c;
        };
        const Fmt f;
        Bytes bigArray = f.tag(kArray);
        bigArray.insert(bigArray.end(), {0xFF, 0xFF, 0xFF, 0x7F});
        rejects("an array of 2 billion elements", withValue(bigArray), "longer than the file");
        Bytes bigDict = f.tag(kDictionary);
        bigDict.insert(bigDict.end(), {0xFF, 0xFF, 0xFF, 0x7F});
        rejects("a dictionary of 2 billion entries", withValue(bigDict), "longer than the file");
        Bytes bigPacked = f.tag(kPackedFloat32);
        bigPacked.insert(bigPacked.end(), {0xFF, 0xFF, 0xFF, 0xFF});
        rejects("a packed float array of 4 billion elements", withValue(bigPacked), "longer than the file");
        Bytes bigBytes = f.tag(kPackedByte);
        bigBytes.insert(bigBytes.end(), {0xFF, 0xFF, 0xFF, 0xFF});
        rejects("a byte array of 4 billion bytes", withValue(bigBytes), "ends early");
        Bytes bigStr = f.tag(kPackedString);
        bigStr.insert(bigStr.end(), {0xFF, 0xFF, 0xFF, 0x7F});
        rejects("a string array of 2 billion strings", withValue(bigStr), "longer than the file");
        Bytes bigString = f.tag(kString);
        bigString.insert(bigString.end(), {0xFF, 0xFF, 0xFF, 0x7F});
        rejects("a string of 2 billion bytes", withValue(bigString), "ends early");
        rejects("an unknown value type", withValue(f.tag(77)), "unknown value type");
        rejects("a legacy input event", withValue(f.tag(25)), "not supported");
        Bytes badObj = f.tag(kObject);
        badObj.insert(badObj.end(), {9, 0, 0, 0});
        rejects("an unknown object kind", withValue(badObj), "object kind");
        rejects("an external resource index past the table", withValue(f.extRef(5)), "external resource index");
        rejects("an internal resource index past the table", withValue(f.subRef(5)), "internal resource index");
        // Nesting: 100 arrays inside arrays.
        Bytes deep;
        for (int i = 0; i < 100; ++i) {
            const Bytes head = f.tag(kArray);
            deep.insert(deep.end(), head.begin(), head.end());
            deep.insert(deep.end(), {1, 0, 0, 0});
        }
        const Bytes leaf = f.nil();
        deep.insert(deep.end(), leaf.begin(), leaf.end());
        rejects("values nested 100 deep", withValue(deep), "nest too deeply");
        // 60 deep is fine.
        Bytes ok60;
        for (int i = 0; i < 60; ++i) {
            const Bytes head = f.tag(kArray);
            ok60.insert(ok60.end(), head.begin(), head.end());
            ok60.insert(ok60.end(), {1, 0, 0, 0});
        }
        ok60.insert(ok60.end(), leaf.begin(), leaf.end());
        GodotResource r;
        GodotDiagnostics dd;
        check(parseRes(withValue(ok60), r, dd), "values nested 60 deep still read");
        // A node path whose string index is past the table.
        rejects("a node path naming a string past the table", withValue(f.nodePath({77}, {}, false)), "string index");
        // The value budget: the element count of an array is bounded by the file size, but the number of Variants in the file is capped too.
        const u32 per = 200000;   // empty arrays in one array; three of those pass the 500,000 cap
        Bytes many = f.tag(kArray);
        for (int i = 0; i < 4; ++i) many.push_back(u8(per >> (8 * i)));
        for (u32 i = 0; i < per; ++i) {
            const Bytes emptyArray = f.tag(kArray);
            many.insert(many.end(), emptyArray.begin(), emptyArray.end());
            many.insert(many.end(), {0, 0, 0, 0});
        }
        Bytes three = f.tag(kArray);
        three.insert(three.end(), {3, 0, 0, 0});
        for (int k = 0; k < 3; ++k) three.insert(three.end(), many.begin(), many.end());
        GodotResource rm;
        GodotDiagnostics dm;
        const bool okMany = parseRes(withValue(three), rm, dm);
        check(!okMany && firstError(dm).find("too many values") != std::string::npos, "more than half a million values in one file is refused: " + firstError(dm));
    }
    // An internal resource whose offset is past the end, or inside the header.
    {
        FileDef d;
        d.cls = "Foo";
        FileDef::Res m;
        m.path = "res://t.res";
        m.type = "Foo";
        d.res.push_back(m);
        Bytes b = d.build();
        const usize offAt = b.size() - 4 - (4 + 4) /* type "Foo\0" */ - 4 /* property count */ - 8;
        Bytes c = b;
        for (int i = 0; i < 8; ++i) c[offAt + usize(i)] = 0xFF;
        rejects("a resource offset past the end of the file", c, "past the end");
        c = b;
        for (int i = 0; i < 8; ++i) c[offAt + usize(i)] = 0;
        c[offAt] = 8;
        rejects("a resource offset inside the header", c, "");
    }
    // Unknown file kinds through the loaders.
    GodotDiagnostics dl;
    GodotResource rl;
    check(!loadGodotResource("C:/definitely/not/here.res", rl, dl) && hasDiag(dl, GodotSeverity::Error, "cannot read"), "a missing file is reported by the loader");
}

// ---- through the project ---------------------------------------------------------------------------------------------------

static void writeFile(const std::string& path, const Bytes& b) { writeFileBytes(path, b.data(), b.size()); }
static void writeText(const std::string& path, const std::string& s) { writeFileBytes(path, s.data(), s.size()); }

static void testProject() {
    AVER_INFO("=== binary files inside a project ===");
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "aver_godot_binary_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "sub", ec);
    const std::string r = root.generic_string();

    writeText(r + "/project.godot", "config_version=5\n\n[application]\n\nconfig/name=\"BinTest\"\n");
    ResFixture a = buildResource(false, false);
    a.d.uid = 0x3d88e35046e8e0c3ULL;
    writeFile(r + "/a.res", a.d.build());
    SceneFixture b = buildScene(false, false);
    b.d.uid = 0x220b6198bd8231fdULL;
    writeFile(r + "/sub/b.scn", b.d.build());
    SceneFixture c = buildScene(false, false);
    c.d.uid = 0x6f607bd40182c27fULL;
    writeFile(r + "/c.scn", wrapRscc(c.d.build(false), 4096));
    writeFile(r + "/bad.res", Bytes{'R', 'S', 'R', 'C', 9, 9, 9, 9, 9, 9, 9, 9, 9, 9});
    writeText(r + "/main.tscn",
              "[gd_scene load_steps=2 format=3]\n\n[ext_resource type=\"PackedScene\" uid=\"uid://ba7lhk44sk366\" path=\"res://stale/b.scn\" id=\"1_b\"]\n\n"
              "[node name=\"Main\" type=\"Node3D\"]\n\n[node name=\"B\" parent=\".\" instance=ExtResource(\"1_b\")]\n");

    GodotProjectFs pfs;
    GodotDiagnostics d;
    check(pfs.open(r, d) && pfs.scan(d), "the project opens and scans");
    check(pfs.binaryResources() == std::vector<std::string>({"res://a.res", "res://bad.res", "res://c.scn", "res://sub/b.scn"}), "the four binary files are listed, sorted");
    check(hasDiag(d, GodotSeverity::Warning, "could not be read") && !hasDiag(d, GodotSeverity::Warning, "resave"), "only the unreadable one is warned about");
    const std::string* ua = pfs.uids().find("uid://b4f5b2j76nf2b");
    const std::string* ub = pfs.uids().find("uid://ba7lhk44sk366");
    const std::string* uc = pfs.uids().find("uid://dmlvwah3ypol0");
    check(ua && *ua == "res://a.res" && ub && *ub == "res://sub/b.scn" && uc && *uc == "res://c.scn", "their uids (one through a compressed first block) are in the uid map");
    GodotResource ra;
    GodotDiagnostics da;
    check(pfs.loadResource("res://a.res", ra, da) && ra.header.type == "Foo" && ra.file == "res://a.res", "loadResource reads a binary .res by path");
    GodotResource ra2;
    check(pfs.loadResource("uid://b4f5b2j76nf2b", ra2, da) && ra2.file == "res://a.res", "and by uid");
    GodotScene sb;
    GodotDiagnostics db;
    check(pfs.loadScene("uid://ba7lhk44sk366", sb, db) && sb.file == "res://sub/b.scn" && sb.nodes.size() == 6, "loadScene reads a binary .scn by uid");
    GodotScene sc;
    check(pfs.loadScene("res://c.scn", sc, db) && sc.nodes.size() == 6, "and a compressed one");
    GodotScene sm;
    GodotDiagnostics dm;
    check(pfs.loadScene("res://main.tscn", sm, dm) && sm.extResources.size() == 1 && pfs.resolve(sm.extResources[0]) == "res://sub/b.scn",
          "a text scene's instance of a binary scene resolves by uid when the written path is stale");
    GodotScene sbad;
    GodotDiagnostics dbad;
    check(!pfs.loadScene("res://bad.res", sbad, dbad) && hasDiag(dbad, GodotSeverity::Error, "could not be read"), "an unreadable binary file is an error naming the file");
    std::string text;
    GodotDiagnostics dt;
    check(!pfs.readText("res://a.res", text, dt) && hasDiag(dt, GodotSeverity::Error, "binary Godot resource"), "readText still refuses a binary file (shaders are text)");
    fs::remove_all(root, ec);
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--dump-stream") {   // writes the scene fixture's compressible stream, for refreshing the pasted samples
        const Bytes s = buildScene(false, false).d.build(false);
        writeFile(argv[2], s);
        return 0;
    }
    testZstd();
    testResource();
    testScene();
    testCompressed();
    testHostile();
    testProject();
    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
