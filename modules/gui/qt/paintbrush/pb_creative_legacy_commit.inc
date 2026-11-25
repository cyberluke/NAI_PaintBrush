QString PaintbrushCreative::commitLegacy()
{
    const bool stageCommit = m_stageCommit;
    m_stageCommit = false;
    if (!hasSubject() || !m_controller)
        return QStringLiteral("Pick a cutout first.");
    if (m_patchId.isEmpty())
        return QStringLiteral("Choose a patch first.");
    if (m_committed)
        return QStringLiteral("Already committed. Pick a patch to start a new run.");
    const PatchDef *def = patchDef(m_patchId);
    if (!def)
        return QStringLiteral("Unknown patch.");

    if (m_replayTimer.isActive())
        m_replayTimer.stop();
    if (m_recording)
        m_recording = false;

    const QSize canvas = m_controller->creativePadCanvasSize();
    if (!canvas.isValid() || canvas.isEmpty())
        return QStringLiteral("Open a canvas first.");

    m_busy = true;
    emit stateChanged();
    QElapsedTimer commitClock;
    commitClock.start();
    const QString group = QStringLiteral("look-%1")
                              .arg(QDateTime::currentMSecsSinceEpoch());
    const bool livingCommit = m_living || stageCommit;
    if (livingCommit)
        ensureLivingPreview();
    const QByteArray stageEnvelope =
        livingCommit ? stageEnvelopeBytes() : QByteArray();

    /* The build is capped so a 4K/8K canvas cannot stall the UI while the
     * look is synthesized. Pieces are authored at the capped stage and mapped
     * onto their real canvas rectangles below; the live preview already runs
     * on a smaller stage, so its geometry is unchanged. */
    constexpr int kMaxBuildSide = 1600;
    const qreal buildScale =
        qMin(1.0, double(kMaxBuildSide)
                      / double(qMax(canvas.width(), canvas.height())));
    const QSize buildStage(
        qMax(1, int(std::lround(canvas.width() * buildScale))),
        qMax(1, int(std::lround(canvas.height() * buildScale))));
    const QRectF buildRect(
        m_subjectCanvasRect.x() * buildScale,
        m_subjectCanvasRect.y() * buildScale,
        m_subjectCanvasRect.width() * buildScale,
        m_subjectCanvasRect.height() * buildScale);

    QImage buildSubject = m_subjectNative;
    const QSize subjectWant(
        qMax(1, int(std::lround(buildRect.width() * 1.25))),
        qMax(1, int(std::lround(buildRect.height() * 1.25))));
    if (buildSubject.width() > subjectWant.width()
        || buildSubject.height() > subjectWant.height())
        buildSubject = buildSubject.scaled(subjectWant, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);

    QVector<CreativePiece> pieces =
        buildPatch(m_patchId, buildSubject, buildRect, buildStage, m_seed);
    /* Motion offsets are proportional to the stage, so bake on the build stage
     * first and map the baked rectangles onto the real canvas afterwards. */
    pieces = bakeFrame(pieces, buildStage);

    /* Rigged Stage: a real Powder prop from the existing GlitterPowder engine
     * (materialState persisted, re-rendered at export scale). It is inserted
     * behind the subject so the living cutout stays clean. */
    if (stageCommit)
    {
        QByteArray powderState;
        const QImage powder = StageRuntime::renderPowderField(
            buildStage, m_seed | 1u, stageLightColor(def->id), &powderState);
        if (!powder.isNull())
        {
            CreativePiece pw;
            pw.name = QStringLiteral("Stage powder");
            pw.role = QStringLiteral("powder");
            pw.image = powder;
            pw.rect = QRectF(QPointF(0, 0), QSizeF(buildStage));
            pw.pivot = pw.rect.center();
            pw.blendMode = BLEND_SCREEN;
            pw.opacity = 0.72f;
            pw.metadata[QStringLiteral("powderPreset")] =
                QStringLiteral("material.glitter.powder");
            pw.metadata[QStringLiteral("powderState")] = powderState;
            int at = 0;
            while (at < pieces.size()
                   && pieces.at(at).role != QLatin1String("subject"))
                ++at;
            pieces.insert(at, pw);
        }
    }

    if (buildScale < 1.0)
    {
        for (CreativePiece &p : pieces)
        {
            p.rect = QRectF(p.rect.x() / buildScale, p.rect.y() / buildScale,
                            p.rect.width() / buildScale,
                            p.rect.height() / buildScale);
            if (!p.pivot.isNull())
                p.pivot = QPointF(p.pivot.x() / buildScale,
                                  p.pivot.y() / buildScale);
        }
    }
    QList<CanvasLayer> layers;
    bool stageSubjectAdded = false;

    for (const CreativePiece &p : pieces)
    {
        /* Stage keeps the ORIGINAL cutout as the subject (non-destructive:
         * the source layer is hidden by the same transaction). */
        if (stageCommit && p.role == QLatin1String("subject"))
        {
            if (!stageSubjectAdded)
            {
                CanvasLayer subject =
                    makeLivingSubjectLayer(group, def->name);
                if (!subject.raster.isNull())
                {
                    layers.append(subject);
                    stageSubjectAdded = true;
                }
            }
            continue;
        }
        if (p.image.isNull())
            continue;
        QPoint offset;
        QImage trimmed;
        if (p.role == QLatin1String("powder"))
        {
            /* Powder materialState is expressed in the full stage/world
             * coordinates; trimming the raster would shift it at export. */
            trimmed = p.image;
        }
        else
        {
            trimmed = creative::trimTransparent(p.image, &offset);
        }
        if (trimmed.isNull())
            continue;
        /* The piece image can be authored below the canvas resolution (capped
         * build stage) or above it (native subject), so map it onto its real
         * canvas rectangle instead of assuming 1 image px == 1 canvas unit. */
        const qreal pxScale = p.rect.width() / qMax(1, p.image.width());
        const QPointF target =
            p.rect.topLeft() + QPointF(offset) * pxScale;
        CanvasLayer layer =
            CanvasLayer::createImage(trimmed, QPointF(0, 0), p.name);
        layer.setTransform(QTransform(pxScale, 0.0, 0.0, pxScale, target.x(),
                                      target.y()));
        layer.type = LAYER_IMAGE;
        layer.opacity = p.opacity;
        layer.blendMode = p.blendMode;
        layer.metadata[QStringLiteral("creative.patch")] = def->id;
        layer.metadata[QStringLiteral("creative.patchName")] = def->name;
        layer.metadata[QStringLiteral("creative.role")] = p.role;
        layer.metadata[QStringLiteral("creative.seed")] = qint64(m_seed);
        layer.metadata[QStringLiteral("creative.group")] = group;
        layer.metadata[QStringLiteral("creative.sourceLayerId")] = m_sourceLayerId;
        layer.metadata[QStringLiteral("creative.sourceHarvestId")] = m_sourceHarvestId;
        QVariantMap params;
        params[QStringLiteral("x")] = double(m_frame.x);
        params[QStringLiteral("y")] = double(m_frame.y);
        params[QStringLiteral("energy")] = double(m_frame.energy);
        params[QStringLiteral("hold")] = m_frame.hold;
        layer.metadata[QStringLiteral("creative.params")] = params;
        /* Scene props carry their procedural role and depth so the committed
         * layer stays self-describing after reopen (Paper Theatre, Nocturne). */
        for (const QString &key : { QStringLiteral("creative.prop"),
                                    QStringLiteral("creative.depth"),
                                    QStringLiteral("creative.parallax") })
            if (p.metadata.contains(key))
                layer.metadata.insert(key, p.metadata.value(key));

        if (p.role == QLatin1String("powder"))
        {
            layer.materialPreset =
                p.metadata.value(QStringLiteral("powderPreset")).toString();
            layer.materialState =
                p.metadata.value(QStringLiteral("powderState")).toByteArray();
        }

        if (stageCommit)
        {
            /* Foreground/background props move at distinct depths on the
             * canonical 3 s clock; accents pulse with the stage light. */
            float depth = 0.0f;
            float parallax = 0.0f;
            float lightPulse = 0.0f;
            if (p.role == QLatin1String("backdrop"))
            {
                depth = -0.65f;
                parallax = 0.60f;
            }
            else if (p.role == QLatin1String("echo"))
            {
                depth = -0.35f;
                parallax = 0.75f;
            }
            else if (p.role == QLatin1String("accent"))
            {
                depth = 0.30f;
                parallax = 0.90f;
                lightPulse = 0.90f;
            }
            else if (p.role == QLatin1String("rim"))
            {
                depth = 0.38f;
                parallax = 1.0f;
                lightPulse = 0.60f;
            }
            else if (p.role == QLatin1String("finish"))
            {
                depth = 0.46f;
                parallax = 0.85f;
                lightPulse = 0.35f;
            }
            else if (p.role == QLatin1String("powder"))
            {
                depth = 0.25f;
                parallax = 0.90f;
            }
            StageRuntime::applyStageMetadata(&layer, group, p.role, depth,
                                             parallax, lightPulse,
                                             stageEnvelope);
        }
        else if (livingCommit && p.role == QLatin1String("subject"))
        {
            /* Photo Regime H: the creative look keeps its exact subject
             * treatment; the derived rig animates that committed cutout. */
            attachDerivedLiving(&layer, group, false);
        }
        layers.append(layer);
    }
    if (stageCommit && !stageSubjectAdded)
    {
        CanvasLayer subject = makeLivingSubjectLayer(group, def->name);
        if (!subject.raster.isNull())
            layers.append(subject);
    }
    if (layers.isEmpty())
    {
        m_busy = false;
        setStatus(QStringLiteral("Nothing to commit — try another patch."));
        return QStringLiteral("The patch produced no layers.");
    }

    int hideLayerId = -1;
    if (m_sourceLayerId > 0)
        hideLayerId = m_sourceLayerId;

    const QString error = m_controller->commitCreativeLayers(
        layers, hideLayerId,
        stageCommit ? QStringLiteral("Rigged Stage: %1").arg(def->name)
                    : QStringLiteral("Create: %1").arg(def->name),
        m_resumeGroup);
    m_busy = false;
    if (!error.isEmpty())
    {
        setStatus(error);
        emit stateChanged();
        return error;
    }

    m_lastCommitMs = double(commitClock.nsecsElapsed()) / 1.0e6;
    m_committed = true;
    m_resumeGroup.clear();
    m_suggestionKind = suggestionKindFor(def->id);
    m_suggestionText = def->suggestion;
    m_suggestionVisible = !m_suggestionText.isEmpty();
    refreshHistory();
    setStatus(stageCommit
                  ? QStringLiteral("Rigged Stage — one undo returns the static cutout.")
                  : (livingCommit
                         ? QStringLiteral("Living look — one undo returns the static cutout.")
                         : QStringLiteral("Committed. Every piece is an editable layer — one undo.")));
    emit suggestionChanged();
    emit metricsChanged();
    emit stateChanged();
    return QString();
}

