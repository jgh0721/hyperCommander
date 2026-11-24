# Keep the pinned KDE sources intact and ship the modified source used to build the plugin.
function(hc_jp2_replace Before After)
    string(FIND "${SourceText}" "${Before}" Position)
    if(Position EQUAL -1)
        message(FATAL_ERROR "The pinned JP2 source no longer matches the scaled decoder changes")
    endif()
    string(REPLACE "${Before}" "${After}" SourceText "${SourceText}")
    set(SourceText "${SourceText}" PARENT_SCOPE)
endfunction()

function(hc_enable_jp2_scaled_decode Source Output)
    file(READ "${Source}" SourceText)
    hc_jp2_replace([=[    QImage readImage(QIODevice *device)
]=] [=[    quint32 maximumReduction() const
    {
        auto info = opj_get_cstr_info(m_jp2_codec);
        if (!info)
            return 0;
        auto reduction = quint32(0);
        const auto *components = info->m_default_tile_info.tccp_info;
        if (components && info->nbcomps) {
            auto resolutions = components[0].numresolutions;
            for (quint32 c = 1; c < info->nbcomps; ++c)
                resolutions = std::min(resolutions, components[c].numresolutions);
            if (resolutions > 1)
                reduction = resolutions - 1;
        }
        opj_destroy_cstr_info(&info);
        return reduction;
    }

    QSize scaledSize() const { return m_scaledSize; }
    void setScaledSize(const QSize &size) { m_scaledSize = size; }

    QImage readImage(QIODevice *device)
]=])
    hc_jp2_replace([=[        auto img = imageAlloc(size(), format());
        if (img.isNull()) {
            return {};
        }

        if (!opj_decode(m_jp2_codec, m_jp2_stream, m_jp2_image)) {
            qCCritical(LOG_JP2PLUGIN) << "Failed to decoding JP2 image!";
            return {};
        }
]=] [=[        auto reducedSize = size();
        const auto outputFormat = format();
        if (reducedSize.isEmpty() || outputFormat == QImage::Format_Invalid) {
            return {};
        }

        if (!m_scaledSize.isEmpty()) {
            const auto limit = maximumReduction();
            quint32 reduction = 0;
            while (reduction < limit) {
                const auto next = reducedSize / 2;
                if (next == reducedSize || next.width() < m_scaledSize.width() || next.height() < m_scaledSize.height())
                    break;
                reducedSize = next;
                ++reduction;
            }
            if (reduction && !opj_set_decoded_resolution_factor(m_jp2_codec, reduction))
                return {};
        }

        if (!opj_decode(m_jp2_codec, m_jp2_stream, m_jp2_image)) {
            qCCritical(LOG_JP2PLUGIN) << "Failed to decoding JP2 image!";
            return {};
        }
        auto img = imageAlloc(size(), outputFormat);
        if (img.isNull())
            return {};
]=])
    hc_jp2_replace([=[        img.setColorSpace(colorSpace());

        return img;
]=] [=[        img.setColorSpace(colorSpace());

        if (!m_scaledSize.isEmpty() && img.size() != m_scaledSize)
            return img.scaled(m_scaledSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        return img;
]=])
    hc_jp2_replace([=[    opj_dparameters_t m_dparameters;
]=] [=[    opj_dparameters_t m_dparameters;
    QSize m_scaledSize;
]=])
    hc_jp2_replace([=[bool JP2Handler::supportsOption(ImageOption option) const
{
]=] [=[bool JP2Handler::supportsOption(ImageOption option) const
{
    if (option == QImageIOHandler::ScaledSize)
        return d->readHeader(device()) && d->maximumReduction() > 0;
]=])
    hc_jp2_replace([=[void JP2Handler::setOption(ImageOption option, const QVariant &value)
{
]=] [=[void JP2Handler::setOption(ImageOption option, const QVariant &value)
{
    if (option == QImageIOHandler::ScaledSize)
        d->setScaledSize(value.toSize());
]=])
    hc_jp2_replace([=[    QVariant v;

    if (option == QImageIOHandler::Size)]=] [=[    QVariant v;

    if (option == QImageIOHandler::ScaledSize)
        return d->scaledSize();
    if (option == QImageIOHandler::Size)]=])
    file(CONFIGURE OUTPUT "${Output}" CONTENT "${SourceText}" @ONLY NEWLINE_STYLE CRLF)
endfunction()
