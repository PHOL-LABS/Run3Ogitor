#include "Run3Document.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <stdexcept>

namespace Run3Editor {
static void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
static QString safeRelative(QString path) {
    path.replace('\\', '/');
    path = QDir::cleanPath(path);
    if(QDir::isAbsolutePath(path) || path == ".." || path.startsWith("../") || path.contains(':'))
        fail("Content path must stay inside its root: " + path);
    return path;
}
static QByteArray read(const QString& path) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) fail("Cannot read " + path + ": " + file.errorString());
    return file.readAll();
}
static void write(const QString& path, const QByteArray& bytes) {
    if(!QDir().mkpath(QFileInfo(path).absolutePath())) fail("Cannot create directory for " + path);
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail("Cannot save " + path + ": " + file.errorString());
}
QStringList Content::roots() const { QStringList result{base}; result.append(overlays); return result; }
QString Content::resolve(const QString& relative) const {
    auto all = roots(); const QString path = safeRelative(relative);
    for(auto it = all.crbegin(); it != all.crend(); ++it) {
        const QString candidate = QDir(*it).filePath(path);
        if(QFileInfo(candidate).isFile()) return QDir::cleanPath(candidate);
    }
    return {};
}
QString Content::destination(const QString& relative) const {
    return QDir(overlays.isEmpty() ? base : overlays.last()).filePath(safeRelative(relative));
}
QMap<QString, QString> Content::files(const QString& mapDirectory) const {
    // Build one merged namespace so an overridden material script is parsed once.
    // Archive entries in resources.cfg are handled separately by the Ogre adapter.
    QStringList dirs{mapDirectory, "run3/core", "run3/models", "run3/mats/high", "run3/shaders",
                     "run3/particle", "run3/trees", "run3/run3shadow", "media"};
    const QString cfg = resolve("run3/core/resources.cfg");
    if(!cfg.isEmpty()) for(auto line : read(cfg).split('\n')) {
        line = line.trimmed();
        if(!line.startsWith("FileSystem=")) continue;
        QString path = QString::fromUtf8(line.mid(11)).trimmed();
        path.replace('\\', '/'); path = QDir::cleanPath(path);
        if(path.contains("/maps/") && path != mapDirectory) continue;
        if(path == "run3" || path == "run3/maps" || path.contains("/game") ||
           path.contains("/lua") || path.contains("/sounds") || path.contains("/fonts") ||
           path.contains("/overlays") || path.contains("/gui") || path == "run3/core") continue;
        dirs.append(safeRelative(path));
    }
    dirs.removeDuplicates();
    QMap<QString, QString> byRelative;
    for(const auto& root : roots()) for(const auto& directory : dirs) {
        QDirIterator it(QDir(root).filePath(directory), QDir::Files, QDirIterator::Subdirectories);
        while(it.hasNext()) {
            const QString file = it.next();
            byRelative[QDir(root).relativeFilePath(file)] = file;
        }
    }
    QMap<QString, QString> result;
    for(auto it = byRelative.begin(); it != byRelative.end(); ++it) {
        const QString suffix = QFileInfo(it.key()).suffix().toLower();
        if(suffix == "overlay" || suffix == "fontdef" || suffix == "xml" || suffix == "cfg" || suffix == "lua") continue;
        const QString name = QFileInfo(it.key()).fileName();
        // Map-local assets take precedence over shared assets with the same name.
        if(!result.contains(name) || it.key().startsWith(mapDirectory + '/')) result[name] = it.value();
    }
    return result;
}
QByteArray printXml(const TiXmlNode& node) {
    TiXmlPrinter printer; printer.SetIndent("    "); node.Accept(&printer);
    return QByteArray(printer.CStr(), int(printer.Size()));
}
void parseXml(TiXmlDocument& document, const QByteArray& bytes) {
    // Native text nodes can contain dialogue or script payloads. TinyXML's
    // default whitespace condensation would silently alter these on editing.
    struct WhitespaceScope {
        bool previous=TiXmlBase::IsWhiteSpaceCondensed();
        WhitespaceScope() { TiXmlBase::SetCondenseWhiteSpace(false); }
        ~WhitespaceScope() { TiXmlBase::SetCondenseWhiteSpace(previous); }
    } scope;
    document.Parse(bytes.constData(),nullptr,TIXML_ENCODING_UTF8);
}
bool propertyElement(const TiXmlElement& element) {
    static const QSet<QString> tags{"position", "rotation", "rotate", "scale", "normal", "axis", "angle",
        "physPosit", "physSize", "physAutoCorrection", "farFind", "colourDiffuse", "colourSpecular",
        "colourAmbient", "lightRange", "lightAttenuation", "clipping", "subentity", "subphys",
        "subnocollide", "lookTarget", "trackTarget", "userData", "userDataReference"};
    return tags.contains(QString::fromUtf8(element.Value()));
}
static void collect(const TiXmlElement& element, const QString& prefix, Fields& result, bool all) {
    for(auto a = element.FirstAttribute(); a; a = a->Next())
        result[prefix + '@' + QString::fromUtf8(a->Name())] = QString::fromUtf8(a->Value());
    int index = 0;
    for(auto n = element.FirstChild(); n; n = n->NextSibling(), ++index) {
        if(n->ToText()) result[prefix + "#text[" + QString::number(index) + ']'] = QString::fromUtf8(n->Value());
        auto e = n->ToElement();
        if(e && (all || propertyElement(*e)))
            collect(*e, prefix + QString::fromUtf8(e->Value()) + '[' + QString::number(index) + "]/", result, true);
    }
}
Fields fields(const TiXmlElement& element) { Fields result; collect(element, {}, result, false); return result; }
void setField(TiXmlElement& element, const QString& key, const QString& value) {
    if(key.startsWith('@')) { element.SetAttribute(key.mid(1).toUtf8().constData(), value.toUtf8().constData()); return; }
    const int a = key.indexOf('['), b = key.indexOf(']');
    if(a < 0 || b < 0) fail("Invalid XML property path: " + key);
    bool ok; int index = key.mid(a + 1, b - a - 1).toInt(&ok);
    if(!ok || index < 0) fail("Invalid XML property index: " + key);
    auto child = element.FirstChild();
    while(child && index-- > 0) child = child->NextSibling();
    if(!child) fail("XML property no longer exists: " + key);
    if(key.startsWith("#text") && child->ToText()) child->SetValue(value.toUtf8().constData());
    else if(child->ToElement()) setField(*child->ToElement(), key.mid(b + 2), value);
    else fail("Invalid XML property target: " + key);
}
void removeField(TiXmlElement& element,const QString& key) {
    if(key.startsWith('@')) { element.RemoveAttribute(key.mid(1).toUtf8().constData()); return; }
    const int a=key.indexOf('['),b=key.indexOf(']');
    bool ok=false; int index=key.mid(a+1,b-a-1).toInt(&ok);
    if(a<0 || b<0 || !ok || index<0) fail("Invalid XML property path: "+key);
    auto node=element.FirstChild(); while(node && index-->0) node=node->NextSibling();
    if(!node) fail("XML property no longer exists: "+key);
    // Leave an empty text node until printing to keep other field indices valid.
    if(key.startsWith("#text") && node->ToText()) node->SetValue("");
    else if(node->ToElement()) removeField(*node->ToElement(),key.mid(b+2));
    else fail("Invalid XML property target: "+key);
}
void Document::open(const QString& sceneCfg, const Content& paths) {
    content = paths; documents.clear(); config.clear(); configRelative.clear();
    auto roots = paths.roots();
    for(auto it = roots.crbegin(); it != roots.crend(); ++it) {
        QString rel = QDir(*it).relativeFilePath(QFileInfo(sceneCfg).absoluteFilePath());
        if(!rel.startsWith("../") && !QDir::isAbsolutePath(rel)) { configRelative = safeRelative(rel); break; }
    }
    if(configRelative.isEmpty()) fail("scene.cfg must be inside the game root or one of its overlays");
    mapDirectory = QFileInfo(configRelative).path();
    configBytes = read(content.resolve(configRelative));
    for(auto line : configBytes.split('\n')) {
        line = line.trimmed(); if(line.isEmpty() || line.startsWith('#') || line.startsWith(';')) continue;
        const int equals = line.indexOf('='); if(equals < 0) continue;
        const QString key = QString::fromUtf8(line.left(equals)).trimmed();
        if(!config.contains(key)) config[key] = QString::fromUtf8(line.mid(equals + 1)).trimmed();
    }
    if(config.contains("BlockManifest") || config.contains("WorldSeed"))
        fail("SemiInfiniteWorlds are not supported by this editor yet. No map files were changed.");
    if(!config.contains("Scene")) fail("scene.cfg does not specify Scene");
    QStringList fileKeys{"Scene", "Scene2", "Sequence"};
    for(const auto& key : fileKeys) {
        const QString value = config.value(key); if(value.isEmpty() || value == "none") continue;
        XmlFile file; file.relative = safeRelative(mapDirectory + '/' + value);
        bool duplicate = false; for(auto& existing : documents) if(existing.relative == file.relative) duplicate = true;
        if(duplicate) continue;
        file.source = content.resolve(file.relative);
        // Legacy TLW configs contain an unused Scene2=2scc.scene on maps where
        // that file was never shipped. Preserve the config, load it if present.
        if(file.source.isEmpty() && key == "Scene2") continue;
        if(file.source.isEmpty()) fail("Missing " + key + " file: " + file.relative);
        file.original = read(file.source);
        file.xml.reset(new TiXmlDocument());
        parseXml(*file.xml,file.original);
        if(file.xml->Error() || !file.xml->RootElement())
            fail(QString("%1:%2: %3").arg(file.source).arg(file.xml->ErrorRow()).arg(file.xml->ErrorDesc()));
        file.canonical = printXml(*file.xml);
        const QString target = content.destination(file.relative);
        file.targetExisted = QFileInfo::exists(target);
        if(file.targetExisted) file.targetOriginal = read(target);
        documents.push_back(std::move(file));
    }
}
void Document::save(const std::vector<QByteArray>& outputs) {
    if(outputs.size() != documents.size()) fail("Run3 document count mismatch");
    struct Pending { QString path; QByteArray data, previous; bool existed; };
    std::vector<Pending> pending;
    for(size_t i = 0; i < outputs.size(); ++i) {
        auto& file = documents[i]; if(outputs[i] == file.canonical) continue;
        TiXmlDocument check; parseXml(check,outputs[i]);
        if(check.Error() || !check.RootElement()) fail("Refusing to save invalid XML: " + file.relative);
        const QString target = content.destination(file.relative);
        if(read(file.source) != file.original) fail("Map changed outside Ogitor. Reopen before saving: " + file.source);
        if(QFileInfo::exists(target) != file.targetExisted || (file.targetExisted && read(target) != file.targetOriginal))
            fail("Overlay changed outside Ogitor. Reopen before saving: " + target);
        pending.push_back({target, outputs[i], file.targetOriginal, file.targetExisted});
    }
    if(pending.empty()) return;
    if(read(content.resolve(configRelative)) != configBytes)
        fail("scene.cfg changed outside Ogitor. Reopen before saving: " + configRelative);
    const QString configTarget = content.destination(configRelative);
    if(!QFileInfo::exists(configTarget)) pending.push_back({configTarget, configBytes, {}, false});
    // Keep recovery copies next to every file being overwritten.
    for(auto& p : pending) if(p.existed) write(p.path + ".ogitor.bak", p.previous);
    size_t committed = 0;
    try { for(auto& p : pending) { write(p.path, p.data); ++committed; } }
    catch(...) {
        for(size_t i = 0; i < committed; ++i) {
            auto& p = pending[i];
            if(p.existed) write(p.path, p.previous); else QFile::remove(p.path);
        }
        throw;
    }
    for(size_t i = 0; i < outputs.size(); ++i) {
        auto& file = documents[i]; if(outputs[i] == file.canonical) continue;
        file.source = content.destination(file.relative); file.original = outputs[i];
        file.canonical = outputs[i]; file.targetOriginal = outputs[i]; file.targetExisted = true;
    }
}
}
