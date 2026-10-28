#include "Run3Mcp.h"
#include "Run3Entity.h"
#include <OgreMeshSerializer.h>
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QToolBar>
#include <QUuid>
#include <QXmlStreamReader>
#include <cmath>

namespace Run3Editor {
using namespace Ogitors;
static void fail(const QString& text) { throw std::runtime_error(text.toStdString()); }
static QString required(const QJsonObject& a, const char* key) {
    if(!a.value(key).isString() || a.value(key).toString().trimmed().isEmpty()) fail(QString("Expected nonempty string: ")+key);
    return a.value(key).toString();
}
static double numeric(const QJsonValue& value, const char* label) {
    if(!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble())>1e9) fail(QString("Invalid finite number: ")+label);
    return value.toDouble();
}
static Ogre::Vector3 vec(const QJsonValue& value, const char* label) {
    if(!value.isArray() || value.toArray().size()!=3) fail(QString("Expected three numbers: ")+label);
    auto a=value.toArray(); return {float(numeric(a[0],label)),float(numeric(a[1],label)),float(numeric(a[2],label))};
}
static QJsonArray json(Ogre::Vector3 v) { return {v.x,v.y,v.z}; }
static QJsonArray json(Ogre::Quaternion q) { return {q.w,q.x,q.y,q.z}; }
static Ogre::Quaternion rotation(const QJsonValue& a) {
    const auto v=vec(a,"rotation_degrees");
    return Ogre::Quaternion(Ogre::Degree(v.y),Ogre::Vector3::UNIT_Y)*Ogre::Quaternion(Ogre::Degree(v.x),Ogre::Vector3::UNIT_X)*Ogre::Quaternion(Ogre::Degree(v.z),Ogre::Vector3::UNIT_Z);
}
static QWidget* renderWidget() {
    for(auto w:QApplication::allWidgets()) if(QByteArray(w->metaObject()->className())=="OgreWidget") return w;
    fail("Render widget is unavailable"); return nullptr;
}
static Entity* entity(int id) {
    if(id<=0) fail("Invalid object ID");
    auto e=dynamic_cast<Entity*>(OgitorsRoot::getSingletonPtr()->FindObject(static_cast<unsigned int>(id)));
    if(!e) fail("Native Run3 object not found; refresh scene_list after undo or reopening");
    return e;
}
static std::vector<Entity*> entities() {
    std::vector<Entity*> all;
    std::function<void(CBaseEditor*)> visit=[&](CBaseEditor* e) {
        if(auto native=dynamic_cast<Entity*>(e)) all.push_back(native);
        for(auto& pair:e->getChildren()) visit(pair.second);
    };
    auto manager=OgitorsRoot::getSingletonPtr()->GetSceneManagerEditor();
    if(manager) visit(manager);
    return all;
}
static QJsonObject describe(Entity* e, bool detail=false) {
    auto xml=e->xml();
    QJsonObject o{{"id",int(e->getObjectID())},{"name",QString::fromStdString(e->getName())},
        {"tag",QString::fromUtf8(xml->Value())},{"parent_id",int(e->getParent()->getObjectID())},
        {"position",json(e->getPosition())},{"world_position",json(e->getDerivedPosition())},
        {"scale",json(e->getScale())},{"orientation_wxyz",json(e->getOrientation())}};
    if(detail) {
        o["xml"]=QString::fromUtf8(printXml(*xml));
        QJsonObject values; auto f=fields(*xml); for(auto it=f.begin();it!=f.end();++it) values[it.key()]=it.value();
        o["fields"]=values;
        auto box=e->getAABB();
        if(!box.isNull() && !box.isInfinite()) o["local_bounds"]=QJsonObject{{"min",json(box.getMinimum())},{"max",json(box.getMaximum())}};
    }
    return o;
}
struct UndoScope {
    UndoScope(const char* name) { OgitorsUndoManager::getSingletonPtr()->BeginCollection(name); }
    ~UndoScope() { OgitorsUndoManager::getSingletonPtr()->EndCollection(true); }
};
static void applyFields(Entity* e, const QJsonObject& values) {
    if(values.isEmpty()) fail("No fields supplied");
    // Validate the entire patch before changing any live property.
    auto xml=e->xml();
    for(auto it=values.begin();it!=values.end();++it) {
        if(!it.value().isString()) fail("XML field values must be strings");
        setField(*xml,it.key(),it.value().toString());
    }
    auto custom=e->getCustomProperties();
    for(auto it=values.begin();it!=values.end();++it) {
        const auto key=it.key().toStdString(),value=it.value().toString().toStdString();
        if(custom->hasProperty(key)) custom->setValue(key,value);
        else custom->addProperty(key,OgitorsPropertyValue(PROP_STRING,Ogre::Any(value)));
    }
}
static void attr(TiXmlElement& e, const char* key, const QString& value) { e.SetAttribute(key,value.toUtf8().constData()); }
static void xyz(TiXmlElement& e, Ogre::Vector3 v) {
    e.SetDoubleAttribute("x",v.x); e.SetDoubleAttribute("y",v.y); e.SetDoubleAttribute("z",v.z);
}
static TiXmlElement* child(TiXmlElement& e, const char* name) { return e.InsertEndChild(TiXmlElement(name))->ToElement(); }
static void writeNew(const QString& file, const QByteArray& data) {
    if(QFileInfo::exists(file)) fail("Asset already exists: "+file);
    QSaveFile out(file);
    if(!out.open(QIODevice::WriteOnly) || out.write(data)!=data.size() || !out.commit()) fail("Cannot write asset: "+file);
}
static QString assetName(const QJsonObject& a) {
    const auto name=required(a,"name");
    if(!QRegularExpression("^[A-Za-z][A-Za-z0-9_-]{0,63}$").match(name).hasMatch()) fail("Asset name must start with a letter and contain only letters, digits, underscore or hyphen (max 64)");
    return "mcp_"+name;
}
McpBridge::McpBridge(McpContext context):mContext(std::move(context)),mServer(new QTcpServer(this)) {
    mToolbar=new QToolBar("Run3 MCP"); mToolbar->setObjectName("Run3McpToolbar");
    mToolbar->addWidget(new QLabel(" Run3 MCP  Port "));
    mPort=new QSpinBox(); mPort->setRange(1024,65535); mPort->setValue(QSettings().value("run3/mcpPort",9877).toInt()); mToolbar->addWidget(mPort);
    mToggle=new QPushButton("Connect"); mToolbar->addWidget(mToggle);
    mStatus=new QLabel(" Disconnected "); mToolbar->addWidget(mStatus);
    connect(mToolbar,&QObject::destroyed,this,[this] { mToolbar=nullptr; stop(); });
    connect(mToggle,&QPushButton::clicked,this,[this] { if(mServer->isListening()) stop(); else start(mPort->value()); });
    connect(mServer,&QTcpServer::newConnection,this,[this] {
        while(mServer->hasPendingConnections()) {
            auto socket=mServer->nextPendingConnection();
            if(mServer->findChildren<QTcpSocket*>().size()>8) { socket->abort(); socket->deleteLater(); continue; }
            socket->setReadBufferSize(1024*1024+1);
            auto buffer=std::make_shared<QByteArray>();
            auto done=std::make_shared<bool>(false);
            QPointer<QTcpSocket> weak(socket);
            auto idle=new QTimer(socket); idle->setSingleShot(true);
            connect(idle,&QTimer::timeout,socket,&QTcpSocket::abort); idle->start(15000);
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket,weak,idle,buffer,done] {
                if(!mToolbar || !mServer->isListening()) { socket->abort(); return; }
                if(*done) return;
                *buffer+=socket->readAll();
                if(buffer->size()>1024*1024) { *done=true; socket->abort(); return; }
                const auto newline=buffer->indexOf('\n'); if(newline<0) return;
                *done=true; idle->stop();
                QJsonObject response;
                try {
                    QJsonParseError error; auto doc=QJsonDocument::fromJson(buffer->left(newline),&error);
                    if(error.error!=QJsonParseError::NoError || !doc.isObject()) fail("Invalid bridge JSON");
                    auto request=doc.object();
                    if(request.value("token").toString()!=mToken) fail("Bridge authentication failed");
                    if(mBusy || QApplication::activeModalWidget()) fail("Editor is busy; close the dialog and retry");
                    QScopedValueRollback<bool> busy(mBusy,true);
                    auto tool=required(request,"tool");
                    mStatus->setText(" Running: "+tool+" ");
                    response={{"ok",true},{"result",dispatch(tool,request.value("arguments").toObject())}};
                    mStatus->setText(" Listening; last: "+tool+" ");
                } catch(const std::exception& error) {
                    response={{"ok",false},{"error",QString::fromUtf8(error.what())}};
                    mStatus->setText(" Request failed (see client) ");
                } catch(...) { response={{"ok",false},{"error","Unknown editor error"}}; }
                if(weak && weak->state()==QAbstractSocket::ConnectedState) {
                    weak->write(QJsonDocument(response).toJson(QJsonDocument::Compact)+'\n'); weak->disconnectFromHost();
                }
            });
        }
    });
    // Explicit opt-in for automated integration tests/launchers only.
    if(qEnvironmentVariableIsSet("RUN3_MCP_AUTOCONNECT")) QTimer::singleShot(0,this,[this] { start(qEnvironmentVariableIntValue("RUN3_MCP_AUTOCONNECT")); });
}
McpBridge::~McpBridge() { stop(); /* MainWindow owns the registered toolbar. */ }
bool McpBridge::start(int port) {
    if(mServer->isListening()) return true;
    if(port<1024 || port>65535 || !mServer->listen(QHostAddress::LocalHost,quint16(port))) { mStatus->setText(" Cannot listen: "+mServer->errorString()); return false; }
    mToken=QUuid::createUuid().toString(QUuid::WithoutBraces)+QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto base=qEnvironmentVariable("LOCALAPPDATA");
    if(base.isEmpty()) base=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    auto directory=base+"/Run3Ogitor"; QDir().mkpath(directory);
    mDescriptor=directory+"/mcp-"+QString::number(port)+".json";
    QSaveFile file(mDescriptor);
    if(!file.open(QIODevice::WriteOnly)) { stop(); mStatus->setText(" Cannot write connection file "); return false; }
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    file.write(QJsonDocument(QJsonObject{{"host","127.0.0.1"},{"port",port},{"token",mToken},{"pid",qint64(QApplication::applicationPid())}}).toJson());
    if(!file.commit()) { stop(); mStatus->setText(" Cannot save connection file "); return false; }
    mPort->setValue(port); mPort->setEnabled(false); mToggle->setText("Disconnect"); mStatus->setText(" Listening on 127.0.0.1:"+QString::number(port)+" ");
    QSettings().setValue("run3/mcpPort",port); return true;
}
void McpBridge::stop() {
    mServer->close();
    for(auto socket:mServer->findChildren<QTcpSocket*>()) socket->abort();
    if(!mDescriptor.isEmpty()) { QFile::remove(mDescriptor); mDescriptor.clear(); }
    mToken.clear();
    if(mToolbar) { mPort->setEnabled(true); mToggle->setText("Connect"); mStatus->setText(" Disconnected "); }
}
QJsonObject McpBridge::dispatch(const QString& tool, const QJsonObject& a) {
    auto root=OgitorsRoot::getSingletonPtr();
    if(tool=="editor_status") {
        auto w=renderWidget();
        auto doc=mContext.document();
        return {{"scene_loaded",root->IsSceneLoaded()},{"native_map",doc?doc->configRelative:QString()},
            {"modified",root->IsSceneModified()},{"render_failed",w->property("renderFailed").toBool()},
            {"units","Run3 world units; Y up; camera looks along local -Z"},
            {"connected",mServer->isListening()},{"port",mPort->value()}};
    }
    if(tool=="level_create" || tool=="level_open" || tool=="level_save") return mContext.lifecycle(tool,a);
    auto document=mContext.document();
    if(!document || !root->IsSceneLoaded()) fail("Open or create a native Run3 level first");
    if(tool=="scene_list") {
        auto all=entities(); QJsonArray out; int matched=0;
        const int offset=std::max(0,a.value("offset").toInt()),limit=std::max(1,std::min(200,a.value("limit").toInt(50)));
        for(auto e:all) {
            auto item=describe(e);
            if(!item["name"].toString().contains(a.value("query").toString(),Qt::CaseInsensitive)) continue;
            if(a.contains("tag") && item["tag"]!=a["tag"]) continue;
            if(matched++>=offset && out.size()<limit) out.append(item);
        }
        return {{"objects",out},{"total",matched},{"offset",offset}};
    }
    if(tool=="object_get") return describe(entity(a.value("id").toInt()),true);
    if(tool=="asset_list") {
        const auto kind=a.value("kind").toString("mesh"),query=a.value("query").toString(); QStringList names;
        if(kind=="material") {
            auto it=Ogre::MaterialManager::getSingleton().getResourceIterator();
            while(it.hasMoreElements()) { auto r=it.getNext(); if(r->getGroup()==PROJECT_RESOURCE_GROUP && !r->getName().empty()) names<<QString::fromStdString(r->getName()); }
        } else {
            if(kind!="mesh" && kind!="texture") fail("kind must be mesh, texture or material");
            auto files=document->content.files(document->mapDirectory);
            for(auto it=files.begin();it!=files.end();++it) {
                const auto suffix=QFileInfo(it.key()).suffix().toLower();
                if((kind=="mesh" && suffix=="mesh") || (kind=="texture" && QStringList{"png","jpg","jpeg","dds","tga","bmp"}.contains(suffix))) names<<it.key();
            }
        }
        names.sort(Qt::CaseInsensitive); names.removeDuplicates(); QJsonArray out; int count=0;
        const int offset=std::max(0,a.value("offset").toInt()),limit=std::max(1,std::min(200,a.value("limit").toInt(50)));
        for(auto name:names) if(name.contains(query,Qt::CaseInsensitive)) { if(count++>=offset && out.size()<limit) out.append(name); }
        return {{"assets",out},{"total",count},{"offset",offset}};
    }
    if(tool=="object_transform") {
        auto e=entity(a.value("id").toInt());
        const auto p=a.contains("position")?vec(a["position"],"position"):e->getPosition();
        const auto s=a.contains("scale")?vec(a["scale"],"scale"):e->getScale();
        if(std::abs(s.x)<1e-6 || std::abs(s.y)<1e-6 || std::abs(s.z)<1e-6) fail("Scale components must be nonzero");
        const auto q=a.contains("rotation_degrees")?rotation(a["rotation_degrees"]):e->getOrientation();
        UndoScope undo("MCP transform"); e->setPosition(p); e->setScale(s); e->setOrientation(q); root->SetSceneModified(true);
        return describe(e);
    }
    if(tool=="object_fields") {
        auto e=entity(a.value("id").toInt()); UndoScope undo("MCP native properties"); applyFields(e,a.value("fields").toObject()); root->SetSceneModified(true); return describe(e,true);
    }
    if(tool=="object_delete") {
        auto e=entity(a.value("id").toInt());
        if(!dynamic_cast<Entity*>(e->getParent())) fail("Cannot delete a document root");
        UndoScope undo("MCP delete"); root->DestroyEditorObject(e,true,true); root->SetSceneModified(true); return {{"deleted",true}};
    }
    if(tool=="history") {
        const auto action=required(a,"action"); auto undo=OgitorsUndoManager::getSingletonPtr();
        if(action=="undo" && undo->CanUndo()) undo->Undo();
        else if(action=="redo" && undo->CanRedo()) undo->Redo();
        else fail("Requested undo/redo is unavailable");
        root->SetSceneModified(true); return {{"can_undo",undo->CanUndo()},{"can_redo",undo->CanRedo()}};
    }
    if(tool=="camera_set") {
        auto camera=root->GetViewport()->getCameraEditor();
        const auto p=a.contains("position")?vec(a["position"],"position"):camera->getDerivedPosition();
        Ogre::Quaternion q=camera->getDerivedOrientation();
        if(a.contains("look_at")) {
            const auto d=vec(a["look_at"],"look_at")-p;
            if(d.squaredLength()<1e-6) fail("Camera and look_at must differ");
            q=Ogre::Vector3::NEGATIVE_UNIT_Z.getRotationTo(d.normalisedCopy());
        } else if(a.contains("rotation_degrees")) q=rotation(a["rotation_degrees"]);
        camera->setDerivedPosition(p); camera->setDerivedOrientation(q);
        return {{"position",json(p)},{"orientation_wxyz",json(q)}};
    }
    if(tool=="viewport_capture") {
        auto widget=renderWidget();
        if(widget->property("renderFailed").toBool()) fail("Rendering is paused; use render_restore and inspect diagnostics");
        if(!widget->isVisible() || widget->window()->isMinimized()) fail("Restore the Ogitor window and select the Render tab before capturing");
        struct GridScope {
            CViewportEditor* viewport;
            bool previous;
            ~GridScope() { viewport->ShowGrid(previous); }
        } grid{root->GetViewport(),root->GetViewport()->IsGridVisible()};
        grid.viewport->ShowGrid(a.value("include_grid").toBool(false));
        widget->repaint();
        if(widget->property("renderFailed").toBool()) fail("Frame failed; use render_restore");
        auto window=root->GetRenderWindow(); const auto width=window->getWidth(),height=window->getHeight();
        if(!width || !height || width>8192 || height>8192) fail("Invalid or oversized render viewport");
        QImage image(int(width),int(height),QImage::Format_RGBA8888);
        Ogre::PixelBox box(width,height,1,Ogre::PF_BYTE_RGBA,image.bits());
        window->copyContentsToMemory(Ogre::Box(0,0,width,height),box,Ogre::RenderTarget::FB_AUTO);
        const auto maxSize=std::max(128,std::min(2048,a.value("max_size").toInt(1280)));
        if(image.width()>maxSize || image.height()>maxSize) image=image.scaled(maxSize,maxSize,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); if(!image.save(&buffer,"PNG")) fail("PNG encoding failed");
        return {{"image_base64",QString::fromLatin1(bytes.toBase64())},{"mimeType","image/png"},{"width",image.width()},{"height",image.height()}};
    }
    if(tool=="render_restore") {
        if(!QMetaObject::invokeMethod(renderWidget(),"restoreRendering",Qt::DirectConnection)) fail("Restore control unavailable");
        renderWidget()->repaint(); return {{"render_failed",renderWidget()->property("renderFailed").toBool()}};
    }
    if(tool=="scene_validate") {
        QJsonArray issues; QSet<QString> names; int meshes=0,lights=0,players=0;
        auto files=document->content.files(document->mapDirectory);
        for(auto e:entities()) {
            auto x=e->xml(); QString tag=QString::fromUtf8(x->Value());
            if(tag=="player") ++players;
            if(tag=="light") ++lights;
            if(x->Attribute("meshFile")) { ++meshes; if(!files.contains(QString::fromUtf8(x->Attribute("meshFile")))) issues.append(QJsonObject{{"id",int(e->getObjectID())},{"issue","Mesh file missing from merged content"}}); }
            if(auto name=x->Attribute("name")) { const auto n=QString::fromUtf8(name); if(names.contains(tag+":"+n)) issues.append(QJsonObject{{"id",int(e->getObjectID())},{"issue","Duplicate game name for tag "+tag}}); names.insert(tag+":"+n); }
        }
        if(!players) issues.append(QJsonObject{{"issue","No player spawn"}});
        return {{"issues",issues},{"mesh_records",meshes},{"lights",lights},{"player_spawns",players},{"render_failed",renderWidget()->property("renderFailed").toBool()},
            {"evaluation","Capture multiple camera angles and visually check layout, scale, textures and lighting. Physics, scripts and gameplay require the Run3 runtime."}};
    }
    if(tool=="material_create") {
        auto name=assetName(a); const auto color=a.contains("color")?vec(a["color"],"color"):Ogre::Vector3::UNIT_SCALE;
        if(color.x<0 || color.y<0 || color.z<0 || color.x>1 || color.y>1 || color.z>1) fail("Material color components must be between 0 and 1");
        auto texture=a.value("texture").toString();
        if(!texture.isEmpty() && (!document->content.files(document->mapDirectory).contains(texture) || texture.contains('"') || texture.contains('\n') || texture.contains('\r'))) fail("Choose an existing texture from asset_list");
        auto path=document->content.destination(document->mapDirectory+'/'+name+".material");
        if(Ogre::MaterialManager::getSingleton().resourceExists(name.toStdString(),PROJECT_RESOURCE_GROUP) || QFileInfo::exists(path)) fail("Material already exists");
        const auto rgb=QString("%1 %2 %3").arg(color.x).arg(color.y).arg(color.z);
        QByteArray script=("material "+name+"\n{\n technique\n {\n  pass\n  {\n   lighting "+QString(a.value("unlit").toBool()?"off":"on")+"\n   ambient "+rgb+"\n   diffuse "+rgb+" 1\n"+(texture.isEmpty()?QString():"   texture_unit\n   {\n    texture \""+texture+"\"\n   }\n")+"  }\n }\n}\n").toUtf8();
        writeNew(path,script);
        try {
            auto stream=Ogre::DataStreamPtr(new Ogre::MemoryDataStream(script.data(),script.size(),false,true));
            Ogre::MaterialManager::getSingleton().parseScript(stream,PROJECT_RESOURCE_GROUP);
        } catch(...) { QFile::remove(path); throw; }
        return {{"material",name},{"file",path},{"note","New map-local asset written; undo changes objects, not asset files"}};
    }
    if(tool=="object_material") {
        auto e=entity(a.value("id").toInt()); const auto mat=required(a,"material");
        if(!Ogre::MaterialManager::getSingleton().resourceExists(mat.toStdString(),PROJECT_RESOURCE_GROUP)) fail("Material not found; use asset_list or material_create");
        // Apply to a mesh record, or to the mesh child of a transform node.
        auto xml=e->xml();
        if(!xml->Attribute("meshFile")) {
            Entity* mesh=nullptr;
            for(auto& pair:e->getChildren()) if(auto native=dynamic_cast<Entity*>(pair.second)) if(native->xml()->Attribute("meshFile")) { if(mesh) fail("Node has several meshes; specify a mesh record ID"); mesh=native; }
            if(!mesh) fail("Object has no mesh"); e=mesh;
        }
        UndoScope undo("MCP material");
        QJsonObject patch;
        auto f=fields(*e->xml()); for(auto it=f.begin();it!=f.end();++it) if(it.key().endsWith("/@materialName")) patch[it.key()]=mat;
        patch["@materialFile"]=mat; applyFields(e,patch); root->SetSceneModified(true); return describe(e,true);
    }
    if(tool=="object_create" || tool=="record_create") {
        const auto all=entities(); Entity* parent=nullptr;
        if(a.contains("parent_id")) parent=entity(a["parent_id"].toInt());
        if(!parent) for(auto e:all) if(QString::fromUtf8(e->xml()->Value())=="nodes") { parent=e; break; }
        if(!parent) fail("Specify a parent_id from scene_list");
        TiXmlDocument xml;
        if(tool=="record_create") {
            const auto bytes=required(a,"xml").toUtf8(); if(bytes.size()>262144) fail("XML record too large");
            // Check nesting without recursion before TinyXML builds its tree.
            QXmlStreamReader reader(bytes); int depth=0,elements=0;
            while(!reader.atEnd()) {
                reader.readNext();
                if(reader.isStartElement() && (++depth>48 || ++elements>2048)) fail("Record exceeds the 48-level/2048-element limit");
                if(reader.isEndElement()) --depth;
                if(reader.isDTD()) fail("DTD declarations are not supported");
            }
            if(reader.hasError()) fail("Invalid record XML: "+reader.errorString());
            parseXml(xml,bytes);
            if(xml.Error() || !xml.RootElement() || xml.RootElement()->NextSiblingElement() || propertyElement(*xml.RootElement())) fail("Expected one native record, not a property element");
            int count=0;
            std::function<void(TiXmlElement*,int)> check=[&](TiXmlElement* e,int depth) {
                if(depth>48 || ++count>2048) fail("Record exceeds the 48-level/2048-element limit");
                for(auto c=e->FirstChildElement();c;c=c->NextSiblingElement()) check(c,depth+1);
            }; check(xml.RootElement(),0);
        } else {
            const auto kind=required(a,"kind"),name=required(a,"name");
            for(auto e:all) { auto x=e->xml(); if(x->Attribute("name") && QString::fromUtf8(x->Attribute("name"))==name) fail("Game object name already exists"); }
            if(kind!="mesh" && kind!="box" && kind!="light") fail("kind must be mesh, box or light");
            const auto p=a.contains("position")?vec(a["position"],"position"):Ogre::Vector3::ZERO;
            const auto s=a.contains("scale")?vec(a["scale"],"scale"):Ogre::Vector3::UNIT_SCALE;
            if(std::abs(s.x)<1e-6 || std::abs(s.y)<1e-6 || std::abs(s.z)<1e-6) fail("Scale must be nonzero");
            const auto q=a.contains("rotation_degrees")?rotation(a["rotation_degrees"]):Ogre::Quaternion::IDENTITY;
            const auto material=a.value("material").toString();
            if(!material.isEmpty() && !Ogre::MaterialManager::getSingleton().resourceExists(material.toStdString(),PROJECT_RESOURCE_GROUP)) fail("Material not found");
            auto node=new TiXmlElement("node"); xml.LinkEndChild(node); attr(*node,"name",name);
            xyz(*child(*node,"position"),p); xyz(*child(*node,"scale"),s);
            auto r=child(*node,"rotation"); r->SetDoubleAttribute("qw",q.w); r->SetDoubleAttribute("qx",q.x); r->SetDoubleAttribute("qy",q.y); r->SetDoubleAttribute("qz",q.z);
            if(kind=="light") {
                auto light=child(*node,"light"); attr(*light,"name",name+"_light"); auto type=a.value("light_type").toString("point");
                if(type!="point" && type!="spot" && type!="directional") fail("Invalid light_type");
                attr(*light,"type",type);
                const auto c=a.contains("color")?vec(a["color"],"color"):Ogre::Vector3::UNIT_SCALE;
                auto col=child(*light,"colourDiffuse"); col->SetDoubleAttribute("r",c.x); col->SetDoubleAttribute("g",c.y); col->SetDoubleAttribute("b",c.z);
                auto att=child(*light,"lightAttenuation"); auto range=a.contains("range")?numeric(a["range"],"range"):1000;
                if(range<=0) fail("Light range must be positive");
                att->SetDoubleAttribute("range",range); att->SetDoubleAttribute("constant",1); att->SetDoubleAttribute("linear",4.5/range); att->SetDoubleAttribute("quadratic",75/(range*range));
            } else {
                QString mesh;
                if(kind=="mesh") {
                    mesh=required(a,"mesh"); if(!mesh.endsWith(".mesh",Qt::CaseInsensitive) || !document->content.files(document->mapDirectory).contains(mesh)) fail("Mesh not found; use asset_list");
                } else {
                    mesh="mcp_unit_box.mesh";
                    const auto path=document->content.destination(document->mapDirectory+'/'+mesh);
                    if(!Ogre::MeshManager::getSingleton().resourceExists(mesh.toStdString(),PROJECT_RESOURCE_GROUP)) {
                        if(QFileInfo::exists(path)) fail("Reopen level to load existing generated box mesh");
                        auto manual=root->GetSceneManager()->createManualObject();
                        try {
                            auto material=Ogre::MaterialManager::getSingleton().getDefaultMaterial(false);
                            manual->begin(material,Ogre::RenderOperation::OT_TRIANGLE_LIST);
                            // Separate face vertices give box lighting correct normals and UVs.
                            const Ogre::Vector3 normal[6]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                            for(auto n:normal) {
                                const auto u=(std::abs(n.y)>0.5f?Ogre::Vector3::UNIT_Z:Ogre::Vector3::UNIT_Y).crossProduct(n).normalisedCopy(),v=n.crossProduct(u);
                                const auto base=manual->getCurrentVertexCount();
                                for(const auto corner:{Ogre::Vector2(-1,-1),Ogre::Vector2(1,-1),Ogre::Vector2(1,1),Ogre::Vector2(-1,1)}) {
                                    manual->position((n+u*corner.x+v*corner.y)*0.5f); manual->normal(n); manual->textureCoord((corner.x+1)*0.5f,(corner.y+1)*0.5f);
                                }
                                manual->triangle(base,base+1,base+2); manual->triangle(base,base+2,base+3);
                            }
                            manual->end(); auto generated=manual->convertToMesh(mesh.toStdString(),PROJECT_RESOURCE_GROUP);
                            Ogre::MeshSerializer().exportMesh(generated.get(),path.toStdString(),Ogre::MESH_VERSION_1_8);
                            root->GetSceneManager()->destroyManualObject(manual);
                        } catch(...) { root->GetSceneManager()->destroyManualObject(manual); throw; }
                    }
                }
                auto object=child(*node,a.value("collision").toBool()?"entity":"nocollide"); attr(*object,"name",name+"_mesh"); attr(*object,"meshFile",mesh);
                if(!material.isEmpty()) attr(*object,"materialFile",material);
            }
        }
        UndoScope undo("MCP create native object");
        auto created=Entity::createTree(*xml.RootElement(),parent,-1,true); root->SetSceneModified(true);
        return describe(created,true);
    }
    if(tool=="environment_set") {
        Entity* environment=nullptr;
        for(auto e:entities()) if(QString::fromUtf8(e->xml()->Value())=="environment") { environment=e; break; }
        if(!environment) fail("Scene has no environment section");
        UndoScope undo("MCP environment");
        if(a.contains("ambient")) {
            const auto c=vec(a["ambient"],"ambient");
            auto xml=environment->xml(); auto f=fields(*xml); QJsonObject patch;
            for(auto it=f.begin();it!=f.end();++it) if(it.key().startsWith("colourAmbient[")) {
                if(it.key().endsWith("/@r")) patch[it.key()]=QString::number(c.x);
                if(it.key().endsWith("/@g")) patch[it.key()]=QString::number(c.y);
                if(it.key().endsWith("/@b")) patch[it.key()]=QString::number(c.z);
            }
            if(patch.isEmpty()) fail("Environment has no colourAmbient; add it to the source scene");
            applyFields(environment,patch);
        }
        if(a.contains("player_position")) {
            const auto p=vec(a["player_position"],"player_position"); Entity* player=nullptr;
            for(auto& pair:environment->getChildren()) if(auto e=dynamic_cast<Entity*>(pair.second)) if(QString::fromUtf8(e->xml()->Value())=="player") { player=e; break; }
            if(!player) { TiXmlElement x("player"); xyz(x,p); player=Entity::createTree(x,environment,-1,true); }
            else player->setPosition(p);
        }
        root->SetSceneModified(true); return describe(environment,true);
    }
    fail("Unknown tool: "+tool); return {};
}
}
