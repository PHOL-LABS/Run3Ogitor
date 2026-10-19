#include "Run3Document.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <iostream>
#include <stdexcept>
using namespace Run3Editor;
static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
static void put(const QString& path,const QByteArray& bytes) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); require(f.open(QIODevice::WriteOnly),"fixture open"); require(f.write(bytes)==bytes.size(),"fixture write"); }
static QByteArray get(const QString& path) { QFile f(path); require(f.open(QIODevice::ReadOnly),"read fixture"); return f.readAll(); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir tmp; require(tmp.isValid(),"temp directory");
        Content paths{tmp.path()+"/base",{tmp.path()+"/overlay"}};
        const QString map="run3/maps/high/test";
        put(paths.base+'/'+map+"/scene.cfg","Scene=scene.xml\nSequence=sequence.xml\n# preserve this\n");
        const QByteArray source="<scene><!--keep--><nodes><node name='a'><position x=1 y=2 z=3/><nocollide meshFile='a.mesh' mystery='yes'/></node></nodes><sounds><sound name='radio'/></sounds><future foo='42'>keep text</future></scene>";
        put(paths.base+'/'+map+"/scene.xml",source);
        put(paths.base+'/'+map+"/sequence.xml","<sequence><adents><npc name='a' autoPosition='true'><physAutoCorrection x='1' y='2' z='3'/></npc><timer name='t' period='4'/></adents><events><event name='e'><changeleveld map='next'/></event></events></sequence>");
        put(paths.overlays[0]+"/run3/models/a.mesh","overlay mesh"); put(paths.base+"/run3/models/a.mesh","base mesh");
        put(paths.base+"/run3/core/approachHighDetail.material","material base {}");
        require(paths.files(map).contains("approachHighDetail.material"),"core material dependencies indexed");
        TiXmlDocument textDoc; parseXml(textDoc,"<caption>keep  double spaces\nand newlines</caption>");
        require(QByteArray(textDoc.RootElement()->GetText())=="keep  double spaces\nand newlines","native text whitespace preserved");
        TiXmlDocument fieldDoc; parseXml(fieldDoc,"<npc extra='yes'><physSize x='1' y='2' z='3'/></npc>");
        removeField(*fieldDoc.RootElement(),"@extra");
        removeField(*fieldDoc.RootElement(),"physSize[0]/@y");
        require(!fieldDoc.RootElement()->Attribute("extra") && !fieldDoc.RootElement()->FirstChildElement()->Attribute("y"),"attribute deletion serialized");
        Document doc; doc.open(paths.base+'/'+map+"/scene.cfg",paths);
        require(doc.documents.size()==2,"load scene and sequence");
        std::vector<QByteArray> outputs; for(auto& f:doc.documents) outputs.push_back(f.canonical);
        doc.save(outputs); require(!QFileInfo::exists(paths.destination(map+"/scene.xml")),"no-op save must not write");
        auto e=doc.documents[0].xml->RootElement()->FirstChildElement("nodes")->FirstChildElement("node");
        auto props=fields(*e); require(props.value("position[0]/@x")=="1","legacy unquoted attributes");
        setField(*e,"position[0]/@x","25"); outputs[0]=printXml(*doc.documents[0].xml);
        doc.save(outputs);
        require(get(paths.base+'/'+map+"/scene.xml")==source,"base remains intact with writable overlay");
        require(get(paths.destination(map+"/scene.xml")).contains("mystery=\"yes\""),"unknown field preserved");
        require(get(paths.destination(map+"/scene.xml")).contains("<!--keep-->"),"comment preserved");
        require(QFileInfo::exists(paths.destination(map+"/scene.cfg")),"overlay cfg created");
        require(paths.files(map).value("a.mesh")==paths.overlays[0]+"/run3/models/a.mesh","resource overlay precedence");
        Document reload; reload.open(paths.destination(map+"/scene.cfg"),paths);
        require(reload.documents[0].source==paths.destination(map+"/scene.xml"),"native reopen resolves overlay");
        auto changed=outputs; changed[0].replace("x=\"25\"","x=\"26\""); doc.save(changed);
        require(QFileInfo::exists(paths.destination(map+"/scene.xml")+".ogitor.bak"),"backup exists");
        put(paths.destination(map+"/scene.xml"),"<scene external='true'/>");
        bool conflict=false; try { doc.save(outputs); } catch(...) { conflict=true; } require(conflict,"external change protected");
        put(paths.base+"/world/scene.cfg","Scene=world.xml\nWorldSeed=123\nBlockManifest=blocks.json\n");
        bool unsupported=false; try { Document world; world.open(paths.base+"/world/scene.cfg",paths); } catch(...) { unsupported=true; } require(unsupported,"semi-infinite rejected");
        bool traversal=false; try { paths.destination("../escape"); } catch(...) { traversal=true; } require(traversal,"path traversal rejected");
        if(argc>1) {
            Content real{QString::fromLocal8Bit(argv[1]),{}};
            QDir high(real.base+"/run3/maps/high"); int loaded=0;
            for(auto dir:high.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
                const QString cfg=high.filePath(dir+"/scene.cfg"); if(!QFileInfo::exists(cfg)) continue;
                Document mapDoc; mapDoc.open(cfg,real); ++loaded;
                std::cout << "Read-only map check: " << dir.toStdString() << " (" << mapDoc.documents.size() << " documents)\n";
            }
            require(loaded>0,"real map inventory empty");
        }
        std::cout << "Run3 document tests passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
