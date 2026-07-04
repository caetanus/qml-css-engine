// CssTheme loading: files/layers/strings, @import inlining (incl. remote fetch + cache),
#include "csstheme.h"
#include "csstheme_p.h"

#include "csslayout.h"
#include "valueparser.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QJSValue>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQuickItem>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVarLengthArray>
#include <QUrl>
#include <algorithm>

namespace QmlCss {

using namespace Detail;

// Inline `@import` statements by reading the referenced sheet and recursively expanding
// it in place. `base` is either the importing sheet's directory (filesystem) or its URL
// (remote sheet): a relative ref resolves against whichever world its parent lives in, so
// a remote theme's relative imports stay remote, like the web. `visited` (absolute paths
// and URLs) guards against import cycles.
//
// Remote sheets are served synchronously FROM THE DISK CACHE when warm; a cold URL is
// fetched asynchronously (fetchRemoteImport) and the load that discovers it completes
// without that chunk — the theme reloads through the same pipeline when the last pending
// fetch lands, and by then the cache is warm, so the splice is synchronous.
QString CssTheme::expandImports(const QString &cssIn, const QString &base, QStringList &visited)
{
    static const QRegularExpression importRe(
        QStringLiteral(R"(@import\s+(?:url\(\s*)?['"]?([^'")\s]+)['"]?\s*\)?\s*;)"));

    const bool baseIsUrl = base.startsWith(QLatin1String("http://"))
        || base.startsWith(QLatin1String("https://"));

    QString out;
    int last = 0;
    auto it = importRe.globalMatch(cssIn);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += cssIn.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();

        QString ref = m.captured(1);
        const bool refIsUrl = ref.startsWith(QLatin1String("http://"))
            || ref.startsWith(QLatin1String("https://"));

        if (refIsUrl || baseIsUrl) {
            const QString url = refIsUrl
                ? ref
                : QUrl(base).resolved(QUrl(ref)).toString();
            if (visited.contains(url))
                continue; // already imported (cycle / duplicate)
            visited.append(url);

            QFile cached(importCachePath(url));
            if (cached.exists() && cached.open(QIODevice::ReadOnly)) {
                // Nested refs inside a remote sheet resolve against ITS url.
                out += expandImports(QString::fromUtf8(cached.readAll()), url, visited);
            } else {
                fetchRemoteImport(url);
            }
            continue;
        }

        if (ref.startsWith(QLatin1String("file://")))
            ref = QUrl(ref).toLocalFile();
        const QString path = QFileInfo(ref).isAbsolute() ? ref : QDir(base).filePath(ref);
        const QString abs = QFileInfo(path).absoluteFilePath();
        if (visited.contains(abs))
            continue; // already imported (cycle / duplicate)
        visited.append(abs);

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "CssTheme @import: cannot read" << path;
            continue;
        }
        const QString sub = QString::fromUtf8(file.readAll());
        out += expandImports(sub, QFileInfo(path).absolutePath(), visited);
    }
    out += cssIn.mid(last);
    return out;
}

// Cold remote @import: download once (async, never blocks the UI thread), park it in the
// disk cache, and re-run the ORIGINAL load when the last in-flight fetch lands — the
// reload's expandImports then splices synchronously from the warm cache. Failures are
// remembered per-URL for this CssTheme's lifetime so a dead URL can't cause a fetch loop.
void CssTheme::fetchRemoteImport(const QString &url)
{
    if (m_importFetchesInFlight.contains(url) || m_importFetchesFailed.contains(url))
        return;
    m_importFetchesInFlight.insert(url);

    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    QNetworkRequest request{ QUrl(url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false); // see @font-face note
    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        m_importFetchesInFlight.remove(url);
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "CssTheme @import: download failed" << url << reply->errorString();
            m_importFetchesFailed.insert(url);
        } else {
            QFile out(importCachePath(url));
            if (out.open(QIODevice::WriteOnly))
                out.write(reply->readAll());
        }
        if (m_importFetchesInFlight.isEmpty())
            reloadCurrentSource();
    });
}

// Re-run the last top-level load (layer or string) through the normal pipeline —
// used when async remote imports finish so the theme picks the fetched chunks up.
void CssTheme::reloadCurrentSource()
{
    switch (m_lastLoadKind) {
    case LastLoad::Layer:
        m_contentHash.clear(); // content may be byte-identical; the imports are what changed
        loadLayered(m_requestedLayer);
        break;
    case LastLoad::String:
        loadFromString(m_lastExternalCss);
        break;
    case LastLoad::None:
        break;
    }
}

void CssTheme::load(const QString &path)
{
    loadLayered(QStringList{path});
}

void CssTheme::setStylePrelude(const QString &css)
{
    m_stylePrelude = css; // applied on the next loadLayered() the caller triggers
}

void CssTheme::loadLayered(const QStringList &paths)
{
    m_requestedLayer = paths; // remembered so a file-change reload re-reads the whole layer
    m_lastLoadKind = LastLoad::Layer;

    QString combined;
    QStringList present;
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "CssTheme: cannot open CSS file" << path;
            continue;
        }
        present.append(path);
        // Inline each file's @imports relative to ITS OWN directory, then concatenate so
        // later sheets (the theme) cascade over earlier ones (the base).
        QStringList visited{QFileInfo(path).absoluteFilePath()};
        combined += expandImports(QString::fromUtf8(file.readAll()),
                                  QFileInfo(path).absolutePath(), visited);
        combined += QLatin1Char('\n');
    }

    // Prepend the config-derived prelude (lowest priority) so the theme CSS — concatenated
    // after it — overrides it on equal specificity. The CSS is thus the single source of truth.
    if (!m_stylePrelude.isEmpty())
        combined = m_stylePrelude + QLatin1Char('\n') + combined;

    // Append the generated layer (highest priority) so synthesised inline-style rules —
    // e.g. `style="..."` props turned into `#id { ... }` — win the cascade over the files.
    if (!m_generatedCss.isEmpty())
        combined += m_generatedCss + QLatin1Char('\n');

    // Watch every present file; drop watches for files no longer in the layer. (Editors
    // atomically replace files, dropping them from the watch list — re-add on each load.)
    const QStringList watched = m_watcher->files();
    for (const QString &w : watched)
        if (!present.contains(w))
            m_watcher->removePath(w);
    for (const QString &p : present)
        if (!m_watcher->files().contains(p))
            m_watcher->addPath(p);

    const QByteArray hash = QCryptographicHash::hash(combined.toUtf8(), QCryptographicHash::Md5);
    if (hash == m_contentHash)
        return; // content unchanged — touch or spurious notification
    m_contentHash = hash;
    m_inInternalLoad = true;
    loadFromString(combined);
    m_inInternalLoad = false;
}

void CssTheme::loadLayeredString(const QString &generatedCss)
{
    // The generated layer is content the build produced (not a file): inline-style rules
    // the transpiler synthesised. Store it and re-run the layered load so it's appended last
    // (highest priority) and survives file-change reloads.
    if (generatedCss == m_generatedCss && m_loaded)
        return;
    m_generatedCss = generatedCss;
    m_contentHash.clear(); // force a rebuild — only the generated layer changed
    loadLayered(m_requestedLayer);
}

void CssTheme::onCssFileChanged(const QString &)
{
    // Any file in the layer changed — re-read the whole layer (base + theme).
    loadLayered(m_requestedLayer);
}

void CssTheme::loadFromString(const QString &cssIn)
{
    QString css = cssIn;
    if (!m_inInternalLoad) {
        // External string load (e.g. a remote root theme applied via set-css): remember it
        // for the async remote-@import reload, and expand its imports — absolute paths and
        // http(s) urls work; a RELATIVE ref has no document to resolve against here.
        m_lastLoadKind = LastLoad::String;
        m_lastExternalCss = cssIn;
        QStringList visited;
        css = expandImports(cssIn, QString(), visited);
    }

    // @keyframes blocks nest, which the flat parseCss can't handle — extract them first
    // (on the comment-stripped, @define-color-expanded source so frame values resolve).
    const QString cleaned = expandDefineColors(stripComments(css));
    m_keyframes.clear();
    const QString afterKeyframes = extractKeyframes(cleaned, m_keyframes);

    // Pull @font-face blocks aside (they nest declarations, like @keyframes) so the flat parser
    // never sees them; each declares a web font to download + register with QFontDatabase.
    QList<CssFontFace> fontFaces;
    const QString afterFonts = extractFontFaces(afterKeyframes, fontFaces);

    // Pull @media blocks aside (and drop @supports/@container); parse the base rules plus each
    // media block's rules. rebuildRules() then assembles the active cascade for the viewport.
    QList<RawMediaBlock> rawMedia;
    const QString body = extractAtRules(afterFonts, rawMedia);
    m_baseRules = parseCss(body);
    m_mediaGroups.clear();
    for (const RawMediaBlock &blk : rawMedia)
        m_mediaGroups.append({ parseMediaCondition(blk.condition), parseCss(blk.body) });

    rebuildRules();
    m_loaded = true;
    emit loadedChanged();
    // Reverse slot: push freshly-resolved styles to every registered object so a theme
    // reload re-styles the live UI without any QML binding.
    if (g_applyStats.enabled) fprintf(stderr, "[reapplyAll] from THEME (RE)LOAD\n");
    reapplyAll();

    // Load/download each @font-face (deduped by URL). Cached fonts register synchronously here;
    // uncached ones arrive later and bump fontRevision, re-resolving the text that uses them.
    for (const CssFontFace &face : fontFaces)
        registerFontFace(face);
}

void CssTheme::rebuildRules()
{
    m_resolveCache.clear();
    m_rules = m_baseRules;
    // Append matching @media rules AFTER the base so they override on equal specificity
    // (CSS source order: @media blocks follow and refine the base).
    for (const CssMediaGroup &group : m_mediaGroups) {
        if (mediaMatches(group.query, m_viewportWidth, m_viewportHeight))
            m_rules += group.rules;
    }

    // Refresh the resize fast-path signature for the CURRENT groups (a theme reload changes
    // the group list, so a stale mask must not skip the next viewport rebuild).
    m_mediaSignature = 0;
    for (int i = 0; i < m_mediaGroups.size() && i < 64; ++i)
        if (mediaMatches(m_mediaGroups.at(i).query, m_viewportWidth, m_viewportHeight))
            m_mediaSignature |= (quint64(1) << i);

    // Rebuild the selector index: a rule is bucketed under ONE key of its subject (a class,
    // else the element, else the id); subject-unconstrained rules go to the unkeyed list.
    // resolveImpl only visits the union of the element's buckets.
    m_rulesByClass.clear();
    m_rulesByElement.clear();
    m_rulesById.clear();
    m_rulesUnkeyed.clear();
    for (int i = 0; i < m_rules.size(); ++i) {
        const CssSimpleSelector &sel = m_rules.at(i).selector;
        if (!sel.classes.isEmpty())
            m_rulesByClass.insert(sel.classes.first(), i);
        else if (!sel.element.isEmpty())
            m_rulesByElement.insert(sel.element, i);
        else if (!sel.id.isEmpty())
            m_rulesById.insert(sel.id, i);
        else
            m_rulesUnkeyed.append(i);
    }
}

void CssTheme::registerFontData(const QByteArray &bytes)
{
    m_fontFamilyCache.clear();
    if (bytes.isEmpty())
        return;
    const int id = QFontDatabase::addApplicationFontFromData(bytes);
    if (id < 0) {
        qWarning() << "CssTheme @font-face: QFontDatabase rejected the font data";
        return;
    }
    // The family is now installed in QFontDatabase, so resolveFontFamily() will find it. Bump the
    // revision (text bindings observe it and re-resolve) and re-push styles to registered objects.
    ++m_fontRevision;
    emit fontRevisionChanged();
    if (g_applyStats.enabled) fprintf(stderr, "[reapplyAll] from FONT REGISTRATION\n");
    reapplyAll();
}

void CssTheme::registerFontFace(const CssFontFace &face)
{
    const QString url = face.url;
    // Dedupe: a font is fetched/registered once, even though loadFromString runs on every reload.
    if (url.isEmpty() || m_fontFacesSeen.contains(url))
        return;
    m_fontFacesSeen.insert(url);

    const bool remote = url.startsWith(QLatin1String("http://")) || url.startsWith(QLatin1String("https://"));
    if (!remote) {
        // A local file:// or filesystem path — register straight from disk.
        const QString path = url.startsWith(QLatin1String("file://")) ? QUrl(url).toLocalFile() : url;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            registerFontData(file.readAll());
        else
            qWarning() << "CssTheme @font-face: cannot read local font" << path;
        return;
    }

    // Disk cache keyed by a hash of the URL, so a launch never re-downloads (fast + offline).
    const QByteArray key = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/webfonts");
    QDir().mkpath(cacheDir);
    const QString cachePath = cacheDir + QLatin1Char('/') + QString::fromLatin1(key);

    QFile cached(cachePath);
    if (cached.exists() && cached.open(QIODevice::ReadOnly)) {
        registerFontData(cached.readAll());
        return;
    }

    // Cache miss: download asynchronously (never block the UI thread). HTTP/2 is disabled to
    // dodge the QNAM connection-reuse bug (see webfetch.cpp).
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    QNetworkRequest request{ QUrl(url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cachePath] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "CssTheme @font-face: download failed" << reply->url().toString()
                       << reply->errorString();
            return;
        }
        const QByteArray fontBytes = reply->readAll();
        if (fontBytes.isEmpty())
            return;
        // Persist to the cache for next launch, then register.
        QFile out(cachePath);
        if (out.open(QIODevice::WriteOnly))
            out.write(fontBytes);
        registerFontData(fontBytes);
    });
}

void CssTheme::setViewport(qreal width, qreal height)
{
    if (qFuzzyCompare(width, m_viewportWidth) && qFuzzyCompare(height, m_viewportHeight))
        return;
    m_viewportWidth = width;
    m_viewportHeight = height;
    emit viewportChanged();
    if (!m_loaded)
        return;
    // Only a change in WHICH @media groups match requires re-resolving styles: a tiling WM
    // retile (or a live resize) that stays within the same breakpoints used to rebuild and
    // re-apply EVERYTHING per resize event. vw/vh lengths are resolved at layout time and
    // follow viewportChanged regardless.
    quint64 signature = 0;
    for (int i = 0; i < m_mediaGroups.size() && i < 64; ++i)
        if (mediaMatches(m_mediaGroups.at(i).query, m_viewportWidth, m_viewportHeight))
            signature |= (quint64(1) << i);
    if (signature == m_mediaSignature)
        return;
    m_mediaSignature = signature;
    rebuildRules();
    if (g_applyStats.enabled) fprintf(stderr, "[reapplyAll] from setViewport(%g, %g)\n", width, height);
    reapplyAll();
}

} // namespace QmlCss
