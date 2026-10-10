#include "gx/pipeline.hpp"

#include <gtest/gtest.h>

TEST(GXPipelineCompatibility, PbrLightmapRequiresSupportedBinding) {
  for (const bool pbr : {false, true}) {
    for (const bool hasLightmapAttribute : {false, true}) {
      for (const bool lightmapBinding : {false, true}) {
        aurora::gx::PipelineConfig config{};
        config.shaderConfig.pbr = pbr;
        config.shaderConfig.pbrLightmapAttr = hasLightmapAttribute ? GX_VA_TEX0 : GX_VA_NULL;

        const bool expected = !pbr || !hasLightmapAttribute || lightmapBinding;
        SCOPED_TRACE(::testing::Message() << "pbr=" << pbr << ", hasLightmapAttribute=" << hasLightmapAttribute
                                          << ", lightmapBinding=" << lightmapBinding);
        EXPECT_EQ(aurora::gx::pipeline_config_compatible(config, lightmapBinding), expected);
      }
    }
  }
}
