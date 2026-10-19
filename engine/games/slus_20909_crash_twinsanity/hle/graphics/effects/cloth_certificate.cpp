#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_certificate.h"

#include <algorithm>
#include <array>

namespace title {
namespace {
using Entry = ClothCertificate::Entry;
using Kind = ClothCertificate::Kind;
const std::array<Entry, 53>& Catalogue() {
    static const std::array<Entry, 53> values{{
// BEGIN GENERATED CLOTH RECORDS
// Generated from the complete verified archive by export_cloth_certificate.py.
// Every consumer is a placed mesh; root still owns all-model and texture selection.
    {0x07004d56u, 0x81a186d7u, Kind::Single, "de1ddaf93d730f117bdd1d2b064d5ff393f4ec826426c4e3fb58d80976dc6879"},
    {0x073c658cu, 0x6de69541u, Kind::Single, "5819451c603c675f8baf67499ce10805f91cefb6dd6017f8985a66bcc8a58076"},
    {0x0866f646u, 0x6de69541u, Kind::Single, "572d33c172938dba68a986235f7b6755cd9b5db745b9c3de41205da6c541fc98"},
    {0x0bd6d59au, 0x6de69541u, Kind::Single, "accdda43a03325bbcfb68356ce2315978e4a6b2e6588ad6468721a80c31db02d"},
    {0x0e6b0188u, 0xbcb6d7c1u, Kind::Single, "3a4cfc4e4d402c718ed64e838f782b4e496c858d8ee94a4f7cffbdae88e67076"},
    {0x0fbf34e5u, 0x40d223c3u, Kind::Single, "8bfd592264d8f02e3e3f2907358f46843a8e8efdda67a256d6b2d56e3f126f87"},
    {0x1a972f07u, 0x3c38971bu, Kind::Single, "812dff39c1ff0da755ea9ba06492a8c8533fe6fbf0b29355949254c7b84f44e1"},
    {0x1a972f07u, 0x9e2d2013u, Kind::Ordinary, "a6a8786f502c7c20be5658f006aeb35709068c21b69f7a0847ecdb04b9670db5"},
    {0x1fd08871u, 0x0c9d9199u, Kind::Single, "b98915a1d4b5bd9b2455eab329ed0333754d4e84148775e4750dac55f34ae668"},
    {0x20cbe159u, 0xf4b5a4ffu, Kind::Single, "07f8e1a6fc081131e2294ce7b3c4fd85e0da65c8dabf0a62a1f6f42624a8763f"},
    {0x20cfc22eu, 0x0c9d9199u, Kind::Single, "7f376bc146cc2917feb310af8cf089ef11a746278d3fc4ba22e9652ccee5971c"},
    {0x2109b9c2u, 0xf86810f3u, Kind::Single, "d51ce63144c7931345ae64143e188d20878aa8ccb3631461db774f0f33426ccf"},
    {0x239c4a13u, 0x6de69541u, Kind::Single, "361de9b36b04bbfdb424334d1ec3c29219a88863f231426d8012d873e9edc33e"},
    {0x2427c4f3u, 0x6de69541u, Kind::Single, "a01447641745046c79d0d58c57d11a050532e98b4eb00a57b6275ebd1ba75d7e"},
    {0x24a15ecbu, 0x8a9c3923u, Kind::Single, "139be8e6c55d92605e6e618ab9eac4a52c953e440eb2804449c31c62145847bc"},
    {0x24c234aau, 0xbcb6d7c1u, Kind::Single, "fd338534d9fefa1c3c804a1404ce2109230a4eafb26e91bd4e4b8f7cff84514f"},
    {0x26675b16u, 0xbcb6d7c1u, Kind::Single, "2fd3c06948d0d796d386c043fb0b81f7e1f0f4bbec9bd1d29a4a64ac06894061"},
    {0x296415d3u, 0x6de69541u, Kind::Single, "71d3bb9d4b61970ac453c6504b8d95d806201c8c51f70170190c14ca20f525a4"},
    {0x2bdb31a5u, 0x40d223c3u, Kind::Single, "004a9b9d6da2e1cdbffc2fd5cba827d21e77830bccd7bb0ad91288c12a185d80"},
    {0x2be838fdu, 0x55a6de28u, Kind::Three, "9d7e6b422d1be258be9addca22e2cd031dd0f460ef98acfa8065236eb212ded8"},
    {0x2d2e6473u, 0x3c38971bu, Kind::Single, "4c93386234b1212be7f0a2f8071d8b70de996265b8a30365820f8bfdfed0e3cf"},
    {0x2d2e6473u, 0x9e2d2013u, Kind::Ordinary, "5728d2e2a27533f4a736104ee76c1bd9136679c2f014377338a70695e08ab45c"},
    {0x33dcb793u, 0x6de69541u, Kind::Single, "47da1a461002dbee3aacc67019c77caf5c0895377d583b65535ef9e302822700"},
    {0x34efca0eu, 0x6de69541u, Kind::Single, "cec94959e4068c683feff9e0d014cfd5e65e9dfde6efb9f8a3e13d081a84a85f"},
    {0x3b18b764u, 0xf4b5a4ffu, Kind::Single, "6db48c34db289a3e4d624d895dd61341d9bbfb588298c8d3c7b9a3a9f5c3ec09"},
    {0x458b85fdu, 0x986b53a9u, Kind::Single, "975819ac2e9c9c147cbd7892f5a402f4132865253abe992b79924e493b19e439"},
    {0x4d052538u, 0x986b53a9u, Kind::Single, "83aac7632ded935626d5b274cdbff3f355995cfd2fd7497e2b98b76696aad807"},
    {0x500a05b0u, 0x6de69541u, Kind::Single, "5a7b9d47fbed29cb882fa20930f9dbc2d4ee9117c0872e6e005f85bbc4596ae7"},
    {0x52674478u, 0x6de69541u, Kind::Single, "090871e4e48251fafe10ab9a716a2f154f805b0e570dd670159883868bb5609d"},
    {0x531e9231u, 0x3c38971bu, Kind::Single, "4cace64ccd98e75036fd1e7308ecd9367519bdacd450bf5980aa99fdf22607dc"},
    {0x531e9231u, 0x9e2d2013u, Kind::Ordinary, "8c8dc99e27c0fe6cae58e628e52d9fc3387a1a5d41cc72988ea673a225864500"},
    {0x546cc19fu, 0xf2272d09u, Kind::Single, "afa864130598c78c827b8eca9119b4aeab5676aa0a1df34c6ae4b5a1522c9bdc"},
    {0x76edfb7du, 0xf4b5a4ffu, Kind::Single, "b9e470e69d07d5fa31f71a11ea48f74234bb7a2794ee2a40f49749180bfeda44"},
    {0x773bea04u, 0x81a186d7u, Kind::Single, "6fbf7d022621b39bded6e458f616da43fd84d280000750129bf1d5ad257c269b"},
    {0x7e679ab8u, 0xf4b5a4ffu, Kind::Single, "bf388a8ed7f569492c29db01fd7cea316e7fc016f14eb1ae4b2721b87202c687"},
    {0x80a21af7u, 0x99771bc9u, Kind::Single, "2f26b7180db967c0e74454cbc214cf4d56842d868f73707ec2225c344f1b1abf"},
    {0x958f8a23u, 0x81a186d7u, Kind::Single, "21033bb236fa27c19e05f273109fe9987509af00436a8c4944a85a0570ce3d74"},
    {0xad93e1e3u, 0xf4b5a4ffu, Kind::Single, "6e9380154b00433ea8d47923b9dec9c219c9c5d675ebfdebdb98f3eb1682ba8a"},
    {0xae63d425u, 0x40d223c3u, Kind::Single, "05c0caa04059868a1e72e48719409675e153384bdf11583a5f185f1531213293"},
    {0xb7b1452cu, 0xf86810f3u, Kind::Single, "c6c444d711a681fab24502df88e876768a54ba157f3b4f6f6694ff66a6184123"},
    {0xbfbfceaau, 0xaa076f6au, Kind::Three, "e0ad5c2b87be8705b2ff282975a9936d0f59793db6f60c2fd72521f79e1dd95b"},
    {0xc70e945fu, 0x99771bc9u, Kind::Single, "52ed07cdc210e0b47d173fc3288e28e8b4be5b0b78906f32237b395b9a180e8b"},
    {0xcda2f8d6u, 0x81a186d7u, Kind::Single, "b6da0f6afd70da63a5c7f5d7039cc537e697751774ed2a0b86999c6a54767109"},
    {0xd2f17247u, 0x99771bc9u, Kind::Single, "b6d144a05f6eec472cea1968806532075465207538577a3d039f90395ec353e7"},
    {0xdc310168u, 0x6de69541u, Kind::Single, "f7a200deb59f1ceec16cb730203ff7641d0a409a865172302238fc5fcf85370f"},
    {0xdcc0290eu, 0xf4b5a4ffu, Kind::Single, "92936b84fbdcf7cb2601e343b24fb739b9ea4b627cc5ff47608120ca5c55a07f"},
    {0xe5165a19u, 0xaa076f6au, Kind::Single, "e15cd5cfb2d47fed4b75c210cbb47369fb435ffad3d66f975e63f98874d57381"},
    {0xe7679747u, 0xaa076f6au, Kind::Three, "7e39d2c4b7d0d74ff503453fc87ba9b724f43a3a44f862504698ba1d7f381a6f"},
    {0xe8825698u, 0xf86810f3u, Kind::Single, "3f72496cad5509f15d104207b3e8008a5cba9a076a6d75df77418b733e28dac8"},
    {0xf1e80d3du, 0x6de69541u, Kind::Single, "dd5471fc5108dd429944f0a8e79032e52ee6adbeda1bb70a83b0db55a5894923"},
    {0xf74a5b60u, 0x6de69541u, Kind::Single, "6901c5008b5764e08e6e9a713a24ce237335e2dd04c38a61621f98e7f568f2cb"},
    {0xfd4e66a2u, 0xbcb6d7c1u, Kind::Single, "c7f577d16e297d3f71516d488991c11c7c36c6abfa7b3d29bcbc4c1672d214b0"},
    {0xff3eb4c3u, 0xaa076f6au, Kind::Three, "6f260dee03060cdd45ad351298c95169fda12e37dcf0e9b4acd3647e7a93e5c9"},
// END GENERATED CLOTH RECORDS
    }};
    return values;
}
}
size_t ClothCertificate::Size() { return Catalogue().size(); }
const ClothCertificate::Entry* ClothCertificate::At(size_t index) {
    return index < Catalogue().size() ? &Catalogue()[index] : nullptr;
}
ClothCertificate::Decision ClothCertificate::Inspect(const Archive& observed, uint32_t material,
                                                    std::string_view digest) {
    const auto expected = MaterialCertificate::RetailArchive();
    if (observed.header_sha256 != expected.header_sha256 || observed.data_sha256 != expected.data_sha256)
        return {Result::UnverifiedArchive};
    const auto& entries = Catalogue();
    auto found = std::lower_bound(entries.begin(), entries.end(), material,
        [](const Entry& entry, uint32_t id) { return entry.material < id; });
    bool known = false;
    for (; found != entries.end() && found->material == material; ++found) {
        known = true;
        if (found->record_sha256 == digest) return {Result::Certified, &*found};
    }
    return {known ? Result::UnknownRecord : Result::UnknownMaterial};
}
}  // namespace title
