#include "Run3Entity.h"
#include "OBBoxRenderable.h"
#include "ofs.h"
#include <OgreArchive.h>
#include <OgreArchiveFactory.h>
#include <OgreArchiveManager.h>
#include <OgreRenderObjectListener.h>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QDateTime>
#include <QTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <cstring>
#include <functional>

namespace Run3Editor {
using namespace Ogitors;
static QByteArray canonicalMesh(const QString& source, const QByteArray& original) {
    if(!source.endsWith(".mesh",Qt::CaseInsensitive) || !original.left(80).contains("[MeshSerializer_v1.4")) return original;
    QString tool=qEnvironmentVariable("RUN3_EDITOR_MESH_UPGRADER");
#ifdef RUN3_LEGACY_MESH_UPGRADER
    if(tool.isEmpty()) tool=QString::fromUtf8(RUN3_LEGACY_MESH_UPGRADER);
#endif
    if(!QFileInfo::exists(tool)) return original;
    const auto key=QCryptographicHash::hash(original+tool.toUtf8()+QByteArray::number(QFileInfo(tool).lastModified().toMSecsSinceEpoch()),QCryptographicHash::Sha256).toHex();
    const QString cache=QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/run3-mesh-v1.8";
    if(!QDir().mkpath(cache)) return original;
    const QString target=cache+'/'+QString::fromLatin1(key)+".mesh";
    if(!QFileInfo::exists(target)) {
        QTemporaryDir temp(cache+"/convert-XXXXXX"); if(!temp.isValid()) return original;
        QProcess process; process.setWorkingDirectory(temp.path());
        process.start(tool,{"-r","-V","1.8","-log",temp.path()+"/conversion.log",source,temp.path()+"/mesh.mesh"});
        if(!process.waitForStarted(3000) || !process.waitForFinished(10000) || process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0) {
            process.kill(); process.waitForFinished(3000);
            Ogre::LogManager::getSingleton().logMessage("Run3 mesh cache: conversion failed for "+source.toStdString());
            return original;
        }
        if(!QFile::copy(temp.path()+"/mesh.mesh",target) && !QFileInfo::exists(target)) return original;
    }
    QFile file(target); return file.open(QIODevice::ReadOnly) ? file.readAll() : original;
}
// One resource namespace with deterministic per-file overlay precedence. The
// Game data stays in place; only incompatible binary mesh versions use a cache.
class Archive : public Ogre::Archive {
public:
    QMap<QString,QString> files;
    Archive(const Ogre::String& name, QMap<QString,QString> entries) : Ogre::Archive(name,"Run3Content"), files(std::move(entries)) {}
    bool isCaseSensitive() const override { return false; }
    void load() override {}
    void unload() override {}
    QString locate(const Ogre::String& name) const {
        const QString key=QString::fromStdString(name);
        auto found=files.find(key); if(found!=files.end()) return found.value();
        for(auto it=files.begin();it!=files.end();++it) if(it.key().compare(key,Qt::CaseInsensitive)==0) return it.value();
        return {};
    }
    Ogre::DataStreamPtr open(const Ogre::String& name, bool readOnly=true) const override {
        QFile file(locate(name));
        if(!readOnly || !file.open(QIODevice::ReadOnly)) OGRE_EXCEPT(Ogre::Exception::ERR_FILE_NOT_FOUND,"Run3 resource not found: "+name,"Run3Content::open");
        auto bytes=canonicalMesh(file.fileName(),file.readAll()); auto stream=Ogre::DataStreamPtr(new Ogre::MemoryDataStream(name,size_t(bytes.size())));
        stream->write(bytes.constData(),size_t(bytes.size())); stream->seek(0); return stream;
    }
    Ogre::StringVectorPtr list(bool recursive=true,bool dirs=false) const override { return find("*",recursive,dirs); }
    Ogre::StringVectorPtr find(const Ogre::String& pattern,bool recursive=true,bool dirs=false) const override {
        auto out=Ogre::StringVectorPtr(new Ogre::StringVector());
        if(!dirs) for(auto it=files.begin();it!=files.end();++it) if(Ogre::StringUtil::match(it.key().toStdString(),pattern,false)) out->push_back(it.key().toStdString());
        return out;
    }
    bool exists(const Ogre::String& name) const override { return !locate(name).isEmpty(); }
    time_t getModifiedTime(const Ogre::String& name) const override { return QFileInfo(locate(name)).lastModified().toSecsSinceEpoch(); }
    Ogre::FileInfoListPtr listFileInfo(bool recursive=true,bool dirs=false) const override { return findFileInfo("*",recursive,dirs); }
    Ogre::FileInfoListPtr findFileInfo(const Ogre::String& pattern,bool recursive=true,bool dirs=false) const override {
        auto out=Ogre::FileInfoListPtr(new Ogre::FileInfoList());
        // Keep the owning shared pointer alive for the whole iteration.
        auto names=find(pattern,recursive,dirs);
        for(auto& name:*names) { Ogre::FileInfo info; info.archive=const_cast<Archive*>(this); info.filename=name; info.basename=name; info.path=""; info.compressedSize=info.uncompressedSize=size_t(QFileInfo(locate(name)).size()); out->push_back(info); }
        return out;
    }
};
class ArchiveFactory : public Ogre::ArchiveFactory {
public:
    QMap<QString,QString> files;
    const Ogre::String& getType() const override { static Ogre::String type="Run3Content"; return type; }
    Ogre::Archive* createInstance(const Ogre::String& name,bool readOnly) override { return OGRE_NEW Archive(name,files); }
    void destroyInstance(Ogre::Archive* archive) override { OGRE_DELETE archive; }
};
static ArchiveFactory* archiveFactory=nullptr;
static QString rootFor(const QString& cfg) {
    const QString absolute=QDir::fromNativeSeparators(QFileInfo(cfg).absoluteFilePath());
    const int run=absolute.indexOf("/run3/",0,Qt::CaseInsensitive);
    return run>=0 ? absolute.left(run) : QFileInfo(cfg).absolutePath();
}
static bool chooseContent(const QString& cfg, Content& paths) {
    QSettings settings;
    QString gameDefault=rootFor(cfg);
    // Infer the base when a map was opened directly in derived-content.
    if(gameDefault.contains("/derived-content/",Qt::CaseInsensitive)) {
        paths.overlays << gameDefault;
        QString engine=gameDefault.left(gameDefault.indexOf("/derived-content/",0,Qt::CaseInsensitive));
        gameDefault=engine+"/Games/The Long Way/TheLongWay";
    }
    QDialog dialog(QApplication::activeWindow()); dialog.setWindowTitle("Open native Run3 map"); dialog.resize(680,330);
    auto layout=new QVBoxLayout(&dialog);
    auto label=new QLabel("Select the game root and optional derived-content overlays.\nLater overlays take priority. Saves go to the last overlay, or the base game when no overlay is set.\n"+cfg); label->setWordWrap(true); layout->addWidget(label);
    auto form=new QFormLayout(); layout->addLayout(form);
    const QString prefix="run3/maps/"+cfg+"/";
    auto base=new QLineEdit(settings.value(prefix+"base",gameDefault).toString());
    auto row=new QHBoxLayout(); row->addWidget(base); auto browse=new QPushButton("Browse..."); row->addWidget(browse); form->addRow("Game content root",row);
    QObject::connect(browse,&QPushButton::clicked,[&] { auto path=QFileDialog::getExistingDirectory(&dialog,"Run3 game content root",base->text()); if(!path.isEmpty()) base->setText(path); });
    auto overlays=new QPlainTextEdit(settings.value(prefix+"overlays",paths.overlays.join('\n')).toString());
    overlays->setPlaceholderText("One overlay directory per line; lowest priority first"); form->addRow("Content overlays",overlays);
    auto add=new QPushButton("Add overlay directory..."); layout->addWidget(add);
    QObject::connect(add,&QPushButton::clicked,[&] { auto path=QFileDialog::getExistingDirectory(&dialog,"Run3 content overlay"); if(!path.isEmpty()) overlays->appendPlainText(path); });
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Open|QDialogButtonBox::Cancel); layout->addWidget(buttons);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted) return false;
    paths.base=QDir::fromNativeSeparators(base->text().trimmed()); paths.overlays.clear();
    if(!QDir(paths.base+"/run3").exists()) throw std::runtime_error("The selected game root must contain a run3 directory");
    for(const auto& path:overlays->toPlainText().split('\n')) if(!path.trimmed().isEmpty()) {
        const QString root=QDir::fromNativeSeparators(path.trimmed());
        if(!QDir(root).exists()) throw std::runtime_error(("Overlay does not exist: "+root).toStdString());
        paths.overlays<<root;
    }
    paths.overlays.removeDuplicates();
    settings.setValue(prefix+"base",paths.base); settings.setValue(prefix+"overlays",paths.overlays.join('\n'));
    return true;
}
class Serializer : public CBaseSerializer {
public:
    Serializer() : CBaseSerializer("Run3 native map (scene.cfg)",CAN_IMPORT|CAN_EXPORT) {}
    Document document;
    std::unique_ptr<QTemporaryDir> workspace;
    std::vector<unsigned int> roots;
    std::vector<unsigned int> editorScaffold;
    Ogre::String archiveName;
    QString fileName;
    bool RequiresTerminateScene() override { return true; }
    bool UsesNativeFiles() const override { return true; }
    bool CanOpenNative(const Ogre::String& file) const override { return QFileInfo(QString::fromStdString(file)).fileName().compare("scene.cfg",Qt::CaseInsensitive)==0; }
    Ogre::String NativeFileName() const override { return fileName.toStdString(); }
    void CloseNative() override {
        if(!archiveName.empty()) { Ogre::ArchiveManager::getSingleton().unload(archiveName); archiveName.clear(); }
        roots.clear(); editorScaffold.clear(); workspace.reset(); document=Document(); fileName.clear();
    }
    Entity* readEntity(TiXmlElement& element,CBaseEditor* parent,int slot) {
        return Entity::createTree(element,parent,slot);
    }
    int Import(Ogre::String importfile="") override {
        auto root=OgitorsRoot::getSingletonPtr();
        try {
            QString cfg=QString::fromStdString(importfile);
            if(cfg.isEmpty()) cfg=QFileDialog::getOpenFileName(QApplication::activeWindow(),"Open Run3 map",{},"Run3 maps (scene.cfg)");
            if(cfg.isEmpty()) return SCF_CANCEL;
            Content paths;
            // Environment overrides support scripted editor smoke tests and launchers.
            if(qEnvironmentVariableIsSet("RUN3_EDITOR_CONTENT_ROOT")) {
                paths.base=qEnvironmentVariable("RUN3_EDITOR_CONTENT_ROOT");
                paths.overlays=qEnvironmentVariable("RUN3_EDITOR_OVERLAYS").split(';',Qt::SkipEmptyParts);
            } else if(!chooseContent(cfg,paths)) return SCF_CANCEL;
            document.open(cfg,paths); // Validate all files before constructing a scene.
            fileName=paths.resolve(document.configRelative);
            workspace.reset(new QTemporaryDir(QDir::tempPath()+"/run3-ogitor-XXXXXX"));
            if(!workspace->isValid()) throw std::runtime_error("Cannot create editor scratch directory");
            root->ClearProjectOptions(); auto options=root->GetProjectOptions();
            options->ProjectDir=workspace->path().toStdString()+"/";
            options->ProjectName=QFileInfo(document.mapDirectory).fileName().toStdString();
            options->SceneManagerName="OctreeSceneManager";
            options->CameraSaveCount=1; options->CameraPositions[0]={0,150,0}; options->CameraOrientations[0]=Ogre::Quaternion::IDENTITY;
            options->AutoBackupEnabled=false;
            if(root->GetProjectFile().mount(workspace->path().toUtf8().constData(),OFS::OFS_MOUNT_OPEN)!=OFS::OFS_OK)
                throw std::runtime_error("Cannot mount the editor scratch directory");
            // Ogitor's ancillary project UI uses OFS; it sees only this temporary
            // directory and can never write generated folders into game content.
            root->PrepareProjectResources();
            archiveFactory->files=paths.files(document.mapDirectory);
            archiveName="Run3Content/"+workspace->path().toStdString();
            auto& resources=Ogre::ResourceGroupManager::getSingleton();
            resources.addResourceLocation(archiveName,"Run3Content",PROJECT_RESOURCE_GROUP);
            const QString rescfg=paths.resolve("run3/core/resources.cfg");
            QFile resourceFile(rescfg);
            if(resourceFile.open(QIODevice::ReadOnly)) for(auto line:resourceFile.readAll().split('\n')) {
                line=line.trimmed(); if(!line.startsWith("Zip=")) continue;
                const QString file=paths.resolve(QString::fromUtf8(line.mid(4)).trimmed());
                if(!file.isEmpty() && !file.endsWith("OgreCore.zip",Qt::CaseInsensitive)) resources.addResourceLocation(file.toStdString(),"Zip",PROJECT_RESOURCE_GROUP);
            }
            // PrepareProjectResources initialized an empty group; clear its script
            // initialization flag so newly registered game scripts are parsed.
            resources.clearResourceGroup(PROJECT_RESOURCE_GROUP);
            resources.initialiseResourceGroup(PROJECT_RESOURCE_GROUP);
            OgitorsPropertyValueMap params;
            params["name"]=OgitorsPropertyValue(PROP_STRING,Ogre::Any(Ogre::String("Run3 Scene")));
            auto manager=root->CreateEditorObject(nullptr,options->SceneManagerName,params,false,false);
            roots.clear();
            for(auto& file:document.documents) {
                auto editor=readEntity(*file.xml->RootElement(),manager,-1); roots.push_back(editor->getObjectID());
            }
            root->AfterLoadScene();
            editorScaffold.clear();
            // Ogitor/plugins create editor-only camera and helper records on
            // load. They are not additions to the native game documents.
            for(auto& pair:manager->getChildren()) if(!dynamic_cast<Entity*>(pair.second))
                editorScaffold.push_back(pair.second->getObjectID());
            root->GetViewport()->getCameraEditor()->setClipDistance({1,100000});
            // Start near the authored player spawn (never run sequence scripts).
            for(auto& file:document.documents) {
                auto element=file.xml->RootElement();
                auto player=element->FirstChildElement("player");
                if(!player) if(auto environment=element->FirstChildElement("environment")) player=environment->FirstChildElement("player");
                if(!player) continue;
                auto p=player->FirstChildElement("position"); if(!p) p=player;
                double x=0,y=100,z=0; p->QueryDoubleAttribute("x",&x); p->QueryDoubleAttribute("y",&y); p->QueryDoubleAttribute("z",&z);
                root->GetViewport()->getCameraEditor()->setPosition({float(x),float(y)+100,float(z)}); break;
            }
            root->AddToRecentFiles(fileName.toStdString());
            root->SetSceneModified(false);
            Ogre::LogManager::getSingleton().logMessage("Run3 native map loaded: "+fileName.toStdString());
            if(qEnvironmentVariableIsSet("RUN3_EDITOR_SMOKE")) QTimer::singleShot(3000,QApplication::instance(),[this,root] {
                try {
                    // Exercise both editing paths, then restore the object. No
                    // source/overlay files are saved by this test.
                    Entity* sample=nullptr;
                    std::function<void(Entity*)> findSample=[&](Entity* entity) {
                        if(sample) return;
                        auto native=entity->xml();
                        if(native->Attribute("name") && native->FirstChildElement("position")) { sample=entity; return; }
                        for(auto& pair:entity->getChildren()) if(auto child=dynamic_cast<Entity*>(pair.second)) findSample(child);
                    };
                    for(auto id:roots) findSample(static_cast<Entity*>(root->FindObject(id)));
                    if(sample) {
                        auto properties=sample->getCustomProperties(); Ogre::String name; properties->getValue("@name",name);
                        const auto position=sample->getPosition(),moved=position+Ogre::Vector3(1,2,3);
                        sample->setPosition(moved); properties->setValue("@name",name+"_smoke");
                        if(sample->getPosition()!=moved) throw std::runtime_error("XML property reset a gizmo edit");
                        auto edited=sample->xml(); double x=0; edited->FirstChildElement("position")->QueryDoubleAttribute("x",&x);
                        if(std::abs(x-moved.x)>0.01) throw std::runtime_error("Gizmo edit was not serialized");
                        properties->setValue("@name",name); sample->setPosition(position);
                    }
                    for(size_t i=0;i<roots.size();++i) {
                        auto entity=dynamic_cast<Entity*>(root->FindObject(roots[i]));
                        TiXmlDocument copy(*document.documents[i].xml); auto xml=entity->xml();
                        copy.ReplaceChild(copy.RootElement(),*xml);
                        if(printXml(copy)!=document.documents[i].canonical) throw std::runtime_error("Native no-edit round trip changed XML");
                    }
                    if(!root->SaveScene()) throw std::runtime_error("Native no-op Save routing failed");
                    // Initial-frame tests miss hover boxes and objects behind
                    // the camera. Exercise the position-only editor geometry
                    // with all three materials, then look around the map.
                    auto cameraEditor=root->GetViewport()->getCameraEditor();
                    auto camera=cameraEditor->getCamera();
                    if(!sample) throw std::runtime_error("No native object available for selection regression test");
                    const auto cameraPosition=cameraEditor->getPosition();
                    const auto cameraOrientation=cameraEditor->getOrientation();
                    const auto target=sample->getDerivedPosition();
                    cameraEditor->setPosition(target+Ogre::Vector3(150,150,150));
                    cameraEditor->setOrientation(Ogre::Vector3::NEGATIVE_UNIT_Z.getRotationTo(Ogre::Vector3(-1,-1,-1).normalisedCopy()));
                    root->GetSelection()->setSelection(sample);
                    for(auto tool : {TOOL_SELECT,TOOL_MOVE,TOOL_ROTATE,TOOL_SCALE}) {
                        root->SetEditorTool(tool);
                        root->Update(0);
                        for(int axis : {0,int(AXIS_X),int(AXIS_Y),int(AXIS_Z),int(AXIS_XY),int(AXIS_YZ),int(AXIS_XZ)}) {
                            root->HighlightGizmo(axis);
                            Ogre::Root::getSingleton().renderOneFrame();
                        }
                    }
                    root->GetSelection()->setSelection(static_cast<CBaseEditor*>(nullptr));
                    root->SetEditorTool(TOOL_SELECT);
                    cameraEditor->setPosition(cameraPosition);
                    cameraEditor->setOrientation(cameraOrientation);
                    root->Update(0);
                    Ogre::LogManager::getSingleton().logMessage("Run3 native smoke: actual selection and move/rotate/scale gizmos with highlighted axes passed");
                    {
                        OBBoxRenderable box;
                        const auto centre=camera->getDerivedPosition()+camera->getDerivedDirection()*100;
                        box.setupVertices(Ogre::AxisAlignedBox(centre-Ogre::Vector3(20),centre+Ogre::Vector3(20)));
                        root->GetSceneManager()->getRootSceneNode()->attachObject(&box);
                        try {
                            for(const char* name : {"SelectionBBMaterial","HighlightBBMaterial","SelectHighlightBBMaterial"}) {
                                box.setMaterial(Ogre::MaterialManager::getSingleton().getByName(name,"General"));
                                Ogre::Root::getSingleton().renderOneFrame();
                            }
                        } catch(...) { box.detachFromParent(); throw; }
                        box.detachFromParent();
                    }
                    const auto orientation=cameraEditor->getOrientation();
                    try {
                        for(int angle=0;angle<360;angle+=30) {
                            cameraEditor->setOrientation(Ogre::Quaternion(Ogre::Degree(angle),Ogre::Vector3::UNIT_Y)*orientation);
                            Ogre::Root::getSingleton().renderOneFrame();
                        }
                    } catch(...) { cameraEditor->setOrientation(orientation); throw; }
                    cameraEditor->setOrientation(orientation);
                    Ogre::LogManager::getSingleton().logMessage("Run3 native smoke: selection/hover boxes and 360-degree camera sweep passed");
                    verifyPreviewVertexLayouts();
                    // Exercise the Qt paint boundary, including a second failed
                    // retry, rather than only calling Ogre directly.
                    QWidget* renderWidget=nullptr;
                    for(auto widget : QApplication::allWidgets())
                        if(QByteArray(widget->metaObject()->className())=="OgreWidget") { renderWidget=widget; break; }
                    if(!renderWidget) throw std::runtime_error("Render widget not found");
                    struct FailingFrame : Ogre::RenderObjectListener {
                        void notifyRenderSingleObject(Ogre::Renderable*, const Ogre::Pass*, const Ogre::AutoParamDataSource*, const Ogre::LightList*, bool) override {
                            throw std::runtime_error("Injected render recovery failure");
                        }
                    } failingFrame;
                    auto scene=root->GetSceneManager();
                    const auto savedPosition=sample->getPosition();
                    root->SetSceneModified(true);
                    const auto viewportCount=root->GetRenderWindow()->getNumViewports();
                    root->ClearScreenBackground(true);
                    scene->addRenderObjectListener(&failingFrame);
                    try {
                        renderWidget->repaint();
                        if(!renderWidget->property("renderFailed").toBool()) throw std::runtime_error("Frame failure was not contained");
                        auto button=renderWidget->findChild<QPushButton*>();
                        if(!button || !button->isVisible()) throw std::runtime_error("Render recovery control is hidden");
                        const auto message=button->parentWidget();
                        renderWidget->repaint();
                        if(!message->isVisible()) throw std::runtime_error("Failure overlay disappeared");
                        button->click();
                        renderWidget->repaint();
                        if(!renderWidget->property("renderFailed").toBool()) throw std::runtime_error("Repeated failure was not contained");
                        scene->removeRenderObjectListener(&failingFrame);
                        button->click();
                        renderWidget->repaint();
                        if(renderWidget->property("renderFailed").toBool() || message->isVisible()) throw std::runtime_error("Rendering did not recover");
                        if(!root->IsSceneModified() || sample->getPosition()!=savedPosition) throw std::runtime_error("Recovery lost unsaved scene state");
                        if(root->GetRenderWindow()->getNumViewports()!=viewportCount) throw std::runtime_error("Failed frame leaked a temporary viewport");
                    } catch(...) { scene->removeRenderObjectListener(&failingFrame); throw; }
                    Ogre::LogManager::getSingleton().logMessage("Run3 native smoke: frame failure, failed retry and successful render recovery passed");
                    Ogre::Root::getSingleton().renderOneFrame();
                    const auto screenshot=qEnvironmentVariable("RUN3_EDITOR_SMOKE");
                    if(screenshot.endsWith(".png")) root->GetRenderWindow()->writeContentsToFile(screenshot.toStdString());
                    Ogre::LogManager::getSingleton().logMessage("Run3 native smoke PASS: rendered frame and lossless editor round trip");
                    root->SetSceneModified(false); QApplication::instance()->setProperty("run3SmokeExit",0); QApplication::exit(0);
                } catch(const std::exception& error) {
                    Ogre::LogManager::getSingleton().logError("Run3 native smoke FAIL: "+Ogre::String(error.what()));
                    root->SetSceneModified(false); QApplication::instance()->setProperty("run3SmokeExit",2); QApplication::exit(2);
                }
            });
            return SCF_OK;
        } catch(const std::exception& error) {
            Ogre::LogManager::getSingleton().logError("Run3 import: "+Ogre::String(error.what()));
            if(qEnvironmentVariableIsSet("RUN3_EDITOR_SMOKE")) {
                QApplication::instance()->setProperty("run3SmokeExit",2);
                QTimer::singleShot(0,QApplication::instance(),[]{ QApplication::exit(2); });
            }
            else QMessageBox::critical(QApplication::activeWindow(),"Run3 map could not be opened",QString::fromUtf8(error.what()));
            root->SetSceneModified(false); root->TerminateScene(); CloseNative(); return SCF_ERRFILE;
        }
    }
    int Export(bool saveAs=false,Ogre::String exportfile="") override {
        if(fileName.isEmpty()) { QMessageBox::information(QApplication::activeWindow(),"Run3","Open a native Run3 map first."); return SCF_CANCEL; }
        if(saveAs || !exportfile.empty()) {
            QMessageBox::information(QApplication::activeWindow(),"Run3 native save","Use Save to write this map in place. To create a variant, reopen it with a different writable content overlay.");
            return SCF_CANCEL;
        }
        try {
            for(auto& pair:OgitorsRoot::getSingletonPtr()->GetSceneManagerEditor()->getChildren())
                if(!dynamic_cast<Entity*>(pair.second) && std::find(editorScaffold.begin(),editorScaffold.end(),pair.second->getObjectID())==editorScaffold.end())
                    throw std::runtime_error("Non-native object '"+pair.second->getName()+"' was added outside the map documents. Use Add Run3 XML child inside a scene/sequence section instead.");
            std::vector<QByteArray> outputs;
            for(size_t i=0;i<roots.size();++i) {
                auto editor=dynamic_cast<Entity*>(OgitorsRoot::getSingletonPtr()->FindObject(roots[i]));
                if(!editor) throw std::runtime_error("A Run3 document root was deleted; undo that deletion before saving");
                TiXmlDocument copy(*document.documents[i].xml);
                auto xml=editor->xml(); copy.ReplaceChild(copy.RootElement(),*xml);
                outputs.push_back(printXml(copy));
            }
            document.save(outputs);
            fileName=document.content.resolve(document.configRelative);
            return SCF_OK;
        } catch(const std::exception& error) {
            if(qEnvironmentVariableIsSet("RUN3_EDITOR_SMOKE")) Ogre::LogManager::getSingleton().logError("Run3 save: "+Ogre::String(error.what()));
            else QMessageBox::critical(QApplication::activeWindow(),"Run3 save failed",QString::fromUtf8(error.what()));
            return SCF_ERRFILE;
        }
    }
};
static Serializer* serializer=nullptr;
}
#if defined(_WIN32)
#define RUN3_EXPORT __declspec(dllexport)
#else
#define RUN3_EXPORT
#endif
extern "C" RUN3_EXPORT bool dllStartPlugin(void* identifier,Ogre::String& name) {
    name="Run3 native map editor";
    Run3Editor::archiveFactory=OGRE_NEW Run3Editor::ArchiveFactory();
    Ogre::ArchiveManager::getSingleton().addArchiveFactory(Run3Editor::archiveFactory);
    Ogitors::OgitorsRoot::getSingletonPtr()->RegisterEditorFactory(identifier,OGRE_NEW Run3Editor::EntityFactory());
    Run3Editor::serializer=OGRE_NEW Run3Editor::Serializer();
    Ogitors::OgitorsRoot::getSingletonPtr()->RegisterSerializer(identifier,Run3Editor::serializer);
    return true;
}
extern "C" RUN3_EXPORT bool dllGetPluginName(Ogre::String& name) { name="Run3 native map editor"; return true; }
extern "C" RUN3_EXPORT bool dllStopPlugin() {
    // ArchiveManager keeps factory pointers until Ogre shutdown, matching OgreOfsPlugin.
    OGRE_DELETE Run3Editor::serializer; Run3Editor::serializer=nullptr; return true;
}
