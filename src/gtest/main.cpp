#include "crypto/common.h"
#include "key.h"
#include "pubkey.h"
#include "util.h"
#include "zcash/JoinSplit.hpp"
#include "gmock/gmock.h"

#include "librustzcash.h"

const std::function<std::string(const char*)> G_TRANSLATION_FUN = nullptr;

struct ECCryptoClosure {
    ECCVerifyHandle handle;
};

ECCryptoClosure instance_of_eccryptoclosure;

ZCJoinSplit* params;

int main(int argc, char** argv)
{
    assert(sodium_init() != -1);
    ECC_Start();

    fs::path sapling_spend = ZC_GetParamsDir() / "sapling-spend.params";
    fs::path sapling_output = ZC_GetParamsDir() / "sapling-output.params";
    fs::path sprout_groth16 = ZC_GetParamsDir() / "sprout-groth16.params";

    static_assert(
        sizeof(boost::filesystem::path::value_type) == sizeof(codeunit),
        "librustzcash not configured correctly");
    auto sapling_spend_str = sapling_spend.native();
    auto sapling_output_str = sapling_output.native();
    auto sprout_groth16_str = sprout_groth16.native();

    librustzcash_init_zksnark_params(
        reinterpret_cast<const codeunit*>(sapling_spend_str.c_str()),
        sapling_spend_str.length(),
        reinterpret_cast<const codeunit*>(sapling_output_str.c_str()),
        sapling_output_str.length(),
        reinterpret_cast<const codeunit*>(sprout_groth16_str.c_str()),
        sprout_groth16_str.length());

    testing::InitGoogleMock(&argc, argv);

    auto ret = RUN_ALL_TESTS();

    ECC_Stop();
    return ret;
}
