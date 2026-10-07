#include "camera-source.hpp"
#include "media-decoder.hpp"

#include <memory>

struct camera_source {
    obs_source_t *source{};
    std::unique_ptr<tazzio::MediaDecoder> decoder;
};

static const char *get_name(void *)
{
    return "Tazzio iPhone Camera";
}

static void *create(obs_data_t *, obs_source_t *source)
{
    auto *context = new camera_source;
    context->source = source;
    obs_source_set_async_unbuffered(source, true);
    context->decoder = std::make_unique<tazzio::MediaDecoder>(source, tazzio::DecoderChannel::IPhoneCamera);
    tazzio::register_camera_decoder(context->decoder.get());
    return context;
}

static void destroy(void *data)
{
    auto *context = static_cast<camera_source *>(data);
    tazzio::unregister_camera_decoder(context->decoder.get());
    delete context;
}

static obs_properties_t *properties(void *)
{
    auto *properties = obs_properties_create();
    obs_properties_add_text(
        properties, "status",
        "Obraz i opcjonalny dźwięk są odbierane z iPhone'a połączonego kodem QR w Docku Tazzio iPhone Camera.",
        OBS_TEXT_INFO);
    return properties;
}

obs_source_info tazzio_iphone_camera_source_info = {
    .id = "tazzio_iphone_camera",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
    .get_name = get_name,
    .create = create,
    .destroy = destroy,
    .get_properties = properties,
};
