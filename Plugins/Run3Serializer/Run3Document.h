#pragma once
#include "tinyxml.h"
#include <QByteArray>
#include <QMap>
#include <QStringList>
#include <memory>
#include <vector>

namespace Run3Editor {
// Ordered roots: base first, then overlays in increasing priority.
struct Content {
    QString base;
    QStringList overlays;
    QString resolve(const QString& relative) const;
    QString destination(const QString& relative) const;
    QStringList roots() const;
    QMap<QString, QString> files(const QString& mapDirectory) const;
};
struct XmlFile {
    QString relative, source;
    QByteArray original, canonical, targetOriginal;
    bool targetExisted = false;
    std::unique_ptr<TiXmlDocument> xml;
};
QByteArray printXml(const TiXmlNode& node);
void parseXml(TiXmlDocument& document, const QByteArray& bytes);
bool propertyElement(const TiXmlElement& element);
using Fields = QMap<QString, QString>;
Fields fields(const TiXmlElement& element);
void setField(TiXmlElement& element, const QString& key, const QString& value);
void removeField(TiXmlElement& element, const QString& key);

class Document {
public:
    Content content;
    QString configRelative, mapDirectory;
    QByteArray configBytes;
    QMap<QString, QString> config;
    std::vector<XmlFile> documents;
    void open(const QString& sceneCfg, const Content& paths);
    // All outputs are validated before any writes. Per-file atomic replacement,
    // durable backups, and rollback of earlier replacements on a later failure.
    void save(const std::vector<QByteArray>& xml);
};
}
