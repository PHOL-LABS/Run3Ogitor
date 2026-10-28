#include "Run3Entity.h"
#include <OgreParticleSystem.h>
#include <OgreMeshSerializer.h>
#include <cmath>
#include <QApplication>
#include <QInputDialog>
#include <QMessageBox>

namespace Run3Editor {
using namespace Ogitors;
static Ogre::String attr(const TiXmlElement& e, const char* name, const Ogre::String& fallback = "") {
    const char* a = e.Attribute(name); return a ? a : fallback;
}
static float number(const TiXmlElement& e, const char* name, float fallback = 0) {
    const char* a = e.Attribute(name); if(!a) return fallback;
    bool ok; float n = QString::fromUtf8(a).toFloat(&ok); return ok && std::isfinite(n) ? n : fallback;
}
static Ogre::Vector3 vector(const TiXmlElement& e, const char* x = "x", const char* y = "y", const char* z = "z", float fallback = 0) {
    return {number(e,x,fallback), number(e,y,fallback), number(e,z,fallback)};
}
static void vectorOut(TiXmlElement& e, Ogre::Vector3 v, const char* x="x", const char* y="y", const char* z="z") {
    e.SetAttribute(x,Ogre::StringConverter::toString(v.x).c_str());
    e.SetAttribute(y,Ogre::StringConverter::toString(v.y).c_str());
    e.SetAttribute(z,Ogre::StringConverter::toString(v.z).c_str());
}
static TiXmlElement* child(TiXmlElement& e, const char* tag) {
    auto c = e.FirstChildElement(tag); return c ? c : e.InsertEndChild(TiXmlElement(tag))->ToElement();
}
static Ogre::MaterialPtr previewMaterial(const Ogre::MaterialPtr& authored) {
    if(!authored) return authored;
    authored->compile();
    // A supported compositor-only 'glow' technique is not a usable scene
    // technique. RTSS needs a supported source in the default scheme.
    for(unsigned short i=0;i<authored->getNumTechniques();++i) {
        auto technique=authored->getTechnique(i);
        if(technique->getSchemeName()==Ogre::MaterialManager::DEFAULT_SCHEME_NAME && technique->isSupported()) return authored;
    }
    auto& materials=Ogre::MaterialManager::getSingleton();
    const auto name="Run3/EditorPreview/"+authored->getName();
    auto result=materials.getByName(name,PROJECT_RESOURCE_GROUP);
    if(result) return result;
    result=materials.create(name,PROJECT_RESOURCE_GROUP);
    // Editor-only fallback for shaders which require the game's renderer.
    // Keep authored material references unchanged when serializing the map.
    if(authored->getNumTechniques() && authored->getTechnique(0)->getNumPasses()) {
        const auto source=authored->getTechnique(0)->getPass(0);
        auto pass=result->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(source->getLightingEnabled());
        pass->setDiffuse(Ogre::ColourValue::White); pass->setAmbient(Ogre::ColourValue::White);
        pass->setSelfIllumination(source->getSelfIllumination());
        pass->setSceneBlending(source->getSourceBlendFactor(),source->getDestBlendFactor());
        pass->setDepthWriteEnabled(source->getDepthWriteEnabled());
        pass->setDepthCheckEnabled(source->getDepthCheckEnabled());
        pass->setCullingMode(source->getCullingMode());
        pass->setAlphaRejectSettings(source->getAlphaRejectFunction(),source->getAlphaRejectValue());
        for(unsigned short i=0;i<source->getNumTextureUnitStates();++i) {
            auto texture=source->getTextureUnitState(i);
            if(texture->getContentType()!=Ogre::TextureUnitState::CONTENT_NAMED || texture->getTextureName().empty() || texture->getTextureType()!=Ogre::TEX_TYPE_2D) continue;
            *pass->createTextureUnitState()=*texture;
            pass->getTextureUnitState(0)->setTextureCoordSet(0);
            break;
        }
    }
    Ogre::LogManager::getSingleton().logMessage("Run3 editor texture preview for unsupported game shader: "+authored->getName());
    return result;
}
static Ogre::MaterialPtr meshPreviewMaterial(Ogre::SubEntity* sub,const Ogre::MeshPtr& mesh) {
    auto material=previewMaterial(sub->getMaterial());
    auto data=sub->getSubMesh()->useSharedVertices ? mesh->sharedVertexData : sub->getSubMesh()->vertexData;
    if(!material || !data) return material;
    const bool normals=data->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL)!=nullptr;
    bool mismatch=false;
    for(unsigned short t=0;t<material->getNumTechniques();++t) {
        auto technique=material->getTechnique(t);
        if(technique->getSchemeName()!=Ogre::MaterialManager::DEFAULT_SCHEME_NAME) continue;
        for(unsigned short p=0;p<technique->getNumPasses();++p) {
            auto pass=technique->getPass(p);
            if(!normals && (pass->getLightingEnabled() || pass->hasVertexProgram())) mismatch=true;
            for(unsigned short u=0;u<pass->getNumTextureUnitStates();++u)
                if(!data->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES,pass->getTextureUnitState(u)->getTextureCoordSet())) mismatch=true;
        }
    }
    if(!mismatch) return material;
    const bool uv0=data->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES,0)!=nullptr;
    const auto name=material->getName()+(uv0?"/EditorUV0":"/EditorNoUV")+(normals?"":"/Unlit");
    auto adapted=Ogre::MaterialManager::getSingleton().getByName(name,PROJECT_RESOURCE_GROUP);
    if(adapted) return adapted;
    // Copy only the source scheme. Cloning RTSS-generated techniques also
    // copies shaders with the incompatible NORMAL/TEXCOORD input layout.
    adapted=Ogre::MaterialManager::getSingleton().create(name,PROJECT_RESOURCE_GROUP);
    auto target=adapted->getTechnique(0);
    target->removeAllPasses();
    Ogre::Technique* source=nullptr;
    for(unsigned short t=0;t<material->getNumTechniques();++t)
        if(material->getTechnique(t)->getSchemeName()==Ogre::MaterialManager::DEFAULT_SCHEME_NAME) { source=material->getTechnique(t); break; }
    if(!source) { target->createPass()->setLightingEnabled(false); return adapted; }
    for(unsigned short p=0;p<source->getNumPasses();++p) {
        auto pass=target->createPass();
        *pass=*source->getPass(p);
        // Regenerate a shader for this vertex layout rather than retaining a
        // custom game program whose inputs may not exist on this submesh.
        pass->setVertexProgram("");
        pass->setFragmentProgram("");
        pass->setGeometryProgram("");
        if(!normals) pass->setLightingEnabled(false);
        if(!uv0) pass->removeAllTextureUnitStates();
        else for(unsigned short u=0;u<pass->getNumTextureUnitStates();++u) pass->getTextureUnitState(u)->setTextureCoordSet(0);
    }
    Ogre::LogManager::getSingleton().logMessage("Run3 editor vertex layout adaptation: "+material->getName()+" on "+mesh->getName());
    return adapted;
}
void verifyPreviewVertexLayouts() {
    auto root=OgitorsRoot::getSingletonPtr();
    auto scene=root->GetSceneManager();
    auto camera=root->GetViewport()->getCameraEditor()->getCamera();
    auto material=Ogre::MaterialManager::getSingleton().create("Run3/SmokeLit",PROJECT_RESOURCE_GROUP);
    // Force both a NORMAL and a TEXCOORD1 dependency in the source material.
    material->getTechnique(0)->getPass(0)->createTextureUnitState()->setTextureCoordSet(1);
    for(bool normals : {false,true}) for(bool uv : {false,true}) {
        auto manual=scene->createManualObject();
        const auto name="Run3/SmokeLayout"+Ogre::String(normals?"N":"P")+(uv?"UV":"");
        Ogre::Entity* entity=nullptr;
        try {
            manual->begin(material,Ogre::RenderOperation::OT_TRIANGLE_LIST);
            for(const auto& point : {Ogre::Vector3(-10,-10,0),Ogre::Vector3(10,-10,0),Ogre::Vector3(0,10,0)}) {
                manual->position(camera->getDerivedPosition()+camera->getDerivedDirection()*100+point);
                if(normals) manual->normal(0,0,1);
                if(uv) manual->textureCoord(0,0);
            }
            manual->triangle(0,1,2); manual->end();
            auto mesh=manual->convertToMesh(name,PROJECT_RESOURCE_GROUP);
            entity=scene->createEntity(mesh); entity->setCastShadows(false);
            auto adapted=meshPreviewMaterial(entity->getSubEntity(0),mesh);
            entity->getSubEntity(0)->setMaterial(adapted);
            scene->getRootSceneNode()->attachObject(entity);
            for(int repeat=0;repeat<2;++repeat) {
                // A reused source may already have generated techniques.
                entity->getSubEntity(0)->setMaterial(meshPreviewMaterial(entity->getSubEntity(0),mesh));
                Ogre::Root::getSingleton().renderOneFrame();
            }
            scene->destroyEntity(entity); entity=nullptr;
            scene->destroyManualObject(manual); manual=nullptr;
            Ogre::MeshManager::getSingleton().remove(name,PROJECT_RESOURCE_GROUP);
        } catch(...) {
            if(entity) scene->destroyEntity(entity);
            if(manual) scene->destroyManualObject(manual);
            throw;
        }
    }
    Ogre::LogManager::getSingleton().logMessage("Run3 native smoke: meshes with missing normals/UVs and reused generated techniques passed");
}
Entity::Entity(CBaseEditorFactory* f) : CNodeEditor(f) {
    PROPERTY_PTR(mXml, "run3::xml", Ogre::String, Ogre::String("<node name=\"new_run3_node\"><position x=\"0\" y=\"0\" z=\"0\"/></node>"), 0, 0);
    PROPERTY_PTR(mSlot, "run3::slot", int, -1, 0, 0);
}
Entity::~Entity() { mCustomProperties.removeListener(this); }
Entity* Entity::createTree(TiXmlElement& element,CBaseEditor* parent,int slot,bool interactive) {
    auto root=OgitorsRoot::getSingletonPtr(); OgitorsPropertyValueMap params;
    QString label=QString::fromUtf8(element.Value());
    const char* name=element.Attribute("name"); if(!name) name=element.Attribute("buttonName");
    if(name) label+=" : "+QString::fromUtf8(name);
    Ogre::String editorName=label.toStdString();
    static unsigned long long nextId=0;
    while(root->FindObject(editorName)) editorName=label.toStdString()+" / "+std::to_string(++nextId);
    params["name"]=OgitorsPropertyValue(PROP_STRING,Ogre::Any(editorName));
    params["run3::xml"]=OgitorsPropertyValue(PROP_STRING,Ogre::Any(printXml(element).toStdString()));
    params["run3::slot"]=OgitorsPropertyValue(PROP_INT,Ogre::Any(slot));
    auto editor=static_cast<Entity*>(root->CreateEditorObject(parent,"Run3 Entity",params,interactive,false));
    int index=0;
    for(auto node=element.FirstChild();node;node=node->NextSibling(),++index)
        if(auto child=node->ToElement()) if(!propertyElement(*child)) createTree(*child,editor,index,interactive);
    if(interactive) editor->load();
    return editor;
}
bool Entity::getObjectContextMenu(UTFStringVector& items) {
    items.clear(); items.push_back("Add Run3 XML child...;:/icons/add.svg");
    return true;
}
void Entity::onObjectContextMenu(int selection) {
    if(selection!=0) return;
    bool accepted=false;
    const auto text=QInputDialog::getMultiLineText(QApplication::activeWindow(),"Add native Run3 record",
        "Enter one XML record. It will be added under the selected section/object.\nIts attributes will appear in Custom Properties. Lua is never executed by the editor.",
        "<node name=\"new_node\"><position x=\"0\" y=\"0\" z=\"0\"/></node>",&accepted);
    if(!accepted) return;
    TiXmlDocument document; parseXml(document,text.toUtf8());
    if(document.Error() || !document.RootElement() || document.RootElement()->NextSiblingElement() || propertyElement(*document.RootElement())) {
        QMessageBox::warning(QApplication::activeWindow(),"Invalid Run3 record","Enter a single entity or section, not a transform/property element."); return;
    }
    auto undo=OgitorsUndoManager::getSingletonPtr(); undo->BeginCollection("Add native Run3 record");
    try { createTree(*document.RootElement(),this,-1,true); mOgitorsRoot->SetSceneModified(true); }
    catch(const std::exception& error) { QMessageBox::warning(QApplication::activeWindow(),"Cannot add record",error.what()); }
    undo->EndCollection(true);
}
void Entity::readTransform(TiXmlElement& e) {
    auto p = e.FirstChildElement("position");
    mInitialPosition = p ? vector(*p) : vector(e);
    if(e.Attribute("pos")) mInitialPosition=Ogre::StringConverter::parseVector3(e.Attribute("pos"));
    auto s = e.FirstChildElement("scale");
    mInitialScale = s ? vector(*s,"x","y","z",1) : vector(e,"sX","sY","sZ",1);
    auto r = e.FirstChildElement("rotation"); if(!r) r = e.FirstChildElement("rotate");
    const TiXmlElement& q = r ? *r : e;
    mInitialOrientation = Ogre::Quaternion(number(q,"qw",1),number(q,"qx"),number(q,"qy"),number(q,"qz"));
    if(e.Attribute("orient")) mInitialOrientation=Ogre::StringConverter::parseQuaternion(e.Attribute("orient"));
    if(auto angle = e.FirstChildElement("angle")) if(auto axis = e.FirstChildElement("axis")) {
        Ogre::Vector3 v = vector(*axis); if(v.squaredLength() > 0) { v.normalise(); mInitialOrientation.FromAngleAxis(Ogre::Degree(number(*angle,"f")),v); }
    }
    if(mInitialOrientation.Norm() > 0.000001f) mInitialOrientation.normalise(); else mInitialOrientation = Ogre::Quaternion::IDENTITY;
    setPosition(mInitialPosition); setScale(mInitialScale); setOrientation(mInitialOrientation);
}
void Entity::createProperties(OgitorsPropertyValueMap& params) {
    CNodeEditor::createProperties(params);
    TiXmlDocument doc; parseXml(doc,QByteArray::fromStdString(mXml->get()));
    if(auto e = doc.RootElement()) {
        auto values = fields(*e);
        for(auto it = values.begin(); it != values.end(); ++it)
            mCustomProperties.addProperty(it.key().toStdString(), OgitorsPropertyValue(PROP_STRING, Ogre::Any(it.value().toStdString())));
        // Explicit transform parameters supplied by clone/undo must win.
        if(params.find("position") == params.end()) readTransform(*e);
        else {
            auto pos = getPosition(), scale = getScale(); auto orientation = getOrientation();
            readTransform(*e); setPosition(pos); setScale(scale); setOrientation(orientation);
        }
    }
    mCustomProperties.addListener(this);
}
std::unique_ptr<TiXmlElement> Entity::xml() {
    TiXmlDocument doc; parseXml(doc,QByteArray::fromStdString(mXml->get()));
    if(doc.Error() || !doc.RootElement()) throw std::runtime_error("Invalid Run3 object XML");
    std::unique_ptr<TiXmlElement> out(static_cast<TiXmlElement*>(doc.RootElement()->Clone()));
    const auto original = fields(*out);
    for(auto property : mCustomProperties.getPropertyVector()) {
        const QString key = QString::fromStdString(property->getName());
        OgitorsPropertyValue val(property->getType(), property->getValue());
        const QString value = QString::fromStdString(OgitorsUtils::GetValueString(val));
        if(original.value(key) != value || !original.contains(key)) setField(*out, key, value);
    }
    for(auto it=original.begin();it!=original.end();++it)
        if(!mCustomProperties.hasProperty(it.key().toStdString())) removeField(*out,it.key());
    if(getPosition() != mInitialPosition) {
        auto p = out->FirstChildElement("position");
        if(out->Attribute("pos")) out->SetAttribute("pos",Ogre::StringConverter::toString(getPosition()).c_str());
        else vectorOut(p ? *p : (out->Attribute("x") ? *out : *child(*out,"position")), getPosition());
    }
    if(getScale() != mInitialScale) {
        auto s = out->FirstChildElement("scale");
        if(!s && out->Attribute("sX")) vectorOut(*out,getScale(),"sX","sY","sZ");
        else vectorOut(*child(*out,"scale"),getScale());
    }
    if(getOrientation() != mInitialOrientation) {
        if(out->Attribute("orient")) out->SetAttribute("orient",Ogre::StringConverter::toString(getOrientation()).c_str());
        else if(auto angle = out->FirstChildElement("angle")) {
            Ogre::Radian rad; Ogre::Vector3 axis; getOrientation().ToAngleAxis(rad,axis);
            vectorOut(*child(*out,"axis"),axis);
            angle->SetAttribute("f",Ogre::StringConverter::toString(rad.valueDegrees()).c_str());
        } else {
            auto r = out->FirstChildElement("rotation"); if(!r) r = out->FirstChildElement("rotate");
            if(!r) r = out->Attribute("qw") ? out.get() : child(*out,"rotation");
            const auto q=getOrientation();
            r->SetAttribute("qw",Ogre::StringConverter::toString(q.w).c_str()); vectorOut(*r,{q.x,q.y,q.z},"qx","qy","qz");
        }
    }
    // Replace child records in place, retaining the original comments and order.
    std::map<int, Entity*> replacements; std::vector<Entity*> additions;
    for(auto& pair : getChildren()) {
        auto e=dynamic_cast<Entity*>(pair.second);
        if(!e) throw std::runtime_error("A non-native Ogitor object was added under a Run3 record. Use Add Run3 XML child instead.");
        if(e->slot() < 0) additions.push_back(e);
        else if(auto existing=replacements.find(e->slot()); existing!=replacements.end()) {
            // Clipboard copies carry their template's slot. Keep the original
            // (older object ID) in place and append the new copy.
            if(existing->second->getObjectID()<e->getObjectID()) additions.push_back(e);
            else { additions.push_back(existing->second); existing->second=e; }
        } else replacements[e->slot()] = e;
    }
    int index = 0;
    for(auto n = out->FirstChild(); n;) {
        auto next = n->NextSibling();
        if(auto e = n->ToElement()) if(!propertyElement(*e)) {
            auto it = replacements.find(index);
            if(it == replacements.end()) out->RemoveChild(n);
            else { auto updated = it->second->xml(); out->ReplaceChild(n,*updated); replacements.erase(it); }
        }
        n = next; ++index;
    }
    for(auto& pair : replacements) additions.push_back(pair.second);
    for(auto e : additions) { auto addition = e->xml(); out->InsertEndChild(*addition); }
    return out;
}
void Entity::OnPropertyChanged(OgitorsPropertySet*, OgitorsPropertyBase*) {
    if(mRefreshing) return;
    mRefreshing = true;
    try {
        TiXmlDocument doc; parseXml(doc,QByteArray::fromStdString(mXml->get()));
        auto e=doc.RootElement();
        if(e) {
            const auto original=fields(*e);
            for(auto property : mCustomProperties.getPropertyVector()) {
                OgitorsPropertyValue value(property->getType(),property->getValue());
                setField(*e,QString::fromStdString(property->getName()),QString::fromStdString(OgitorsUtils::GetValueString(value)));
            }
            const auto& currentProperties=mCustomProperties.getPropertyVector();
            for(auto it=original.begin();it!=original.end();++it)
                // During OnPropertyRemoved the vector is already updated, but
                // the property map still contains the entry until callbacks end.
                if(std::none_of(currentProperties.begin(),currentProperties.end(),[&](OgitorsPropertyBase* property) { return property->getName()==it.key().toStdString(); })) removeField(*e,it.key());
            const auto p=mInitialPosition,s=mInitialScale; const auto q=mInitialOrientation;
            const auto currentP=getPosition(),currentS=getScale(); const auto currentQ=getOrientation();
            readTransform(*e);
            // An unrelated XML property must not undo a gizmo edit. A newly
            // edited transform field, however, takes precedence for that field.
            if(mInitialPosition==p) setPosition(currentP);
            if(mInitialScale==s) setScale(currentS);
            if(mInitialOrientation==q) setOrientation(currentQ);
            if(mLoaded->get()) { clearPreview(); preview(*e); }
        }
    } catch(const std::exception& e) { Ogre::LogManager::getSingleton().logError(e.what()); }
    mRefreshing = false;
}
void Entity::OnPropertySetRebuilt(OgitorsPropertySet* set) { OnPropertyChanged(set,nullptr); }
bool Entity::load(bool async) {
    if(mLoaded->get()) return true;
    if(!CNodeEditor::load(false)) return false;
    TiXmlDocument doc; parseXml(doc,QByteArray::fromStdString(mXml->get()));
    if(auto element=doc.RootElement()) {
        const Ogre::String tag=element->Value();
        if(tag=="frame" || tag=="keyPoint") {
            // Sequence path points are world-space, despite XML nesting.
            mHandle->getParentSceneNode()->removeChild(mHandle);
            mOgitorsRoot->GetSceneManager()->getRootSceneNode()->addChild(mHandle);
        }
        preview(*element);
    }
    return true;
}
void Entity::clearPreview() {
    auto manager = mOgitorsRoot->GetSceneManager();
    if(mMesh) { manager->destroyEntity(mMesh); mMesh=nullptr; }
    if(mLines) { manager->destroyManualObject(mLines); mLines=nullptr; }
    if(mLight) { manager->destroyLight(mLight); mLight=nullptr; }
    if(mParticles) { manager->destroyParticleSystem(mParticles); mParticles=nullptr; }
}
bool Entity::unLoad() { if(mLoaded->get()) clearPreview(); return CNodeEditor::unLoad(); }
Ogre::AxisAlignedBox Entity::getAABB() {
    if(mMesh) return mMesh->getBoundingBox();
    if(mLines) return mLines->getBoundingBox();
    return Ogre::AxisAlignedBox({-16,-16,-16},{16,16,16});
}
void Entity::preview(TiXmlElement& e) {
    auto manager = mOgitorsRoot->GetSceneManager();
    const Ogre::String tag = e.Value();
    if(tag=="environment") if(auto c=e.FirstChildElement("colourAmbient"))
        manager->setAmbientLight(Ogre::ColourValue(number(*c,"r",0.25f),number(*c,"g",0.25f),number(*c,"b",0.25f)));
    Ogre::String mesh = attr(e,"meshFile",attr(e,"mesh",attr(e,"meshName")));
    // Wrapper buttons often contain a mesh child; render it only once.
    if(e.FirstChildElement("entity") || e.FirstChildElement("nocollide")) mesh.clear();
    try {
        if(!mesh.empty()) {
            // TLW assets predate Ogre 1.8. Opt in only while importing a selected
            // local game mesh; do not enable legacy binary readers globally.
            struct LegacyMeshScope {
                LegacyMeshScope() { Ogre::MeshSerializer::enablePre1_8Formats(true); }
                ~LegacyMeshScope() { Ogre::MeshSerializer::enablePre1_8Formats(false); }
            } legacyMeshScope;
            mMesh = manager->createEntity(getName(),mesh,PROJECT_RESOURCE_GROUP);
            mMesh->setCastShadows(false); mMesh->setQueryFlags(QUERYFLAG_MOVABLE); mHandle->attachObject(mMesh);
            const auto material = attr(e,"materialFile",attr(e,"materialName"));
            if(!material.empty() && Ogre::MaterialManager::getSingleton().resourceExists(material,PROJECT_RESOURCE_GROUP)) mMesh->setMaterialName(material,PROJECT_RESOURCE_GROUP);
            for(auto sub = e.FirstChildElement(); sub; sub=sub->NextSiblingElement()) {
                const auto mat = attr(*sub,"materialName"); const int index=int(number(*sub,"index",-1));
                if(index >= 0 && index < int(mMesh->getNumSubEntities()) && !mat.empty()) mMesh->getSubEntity(index)->setMaterialName(mat,PROJECT_RESOURCE_GROUP);
            }
            for(unsigned int i=0;i<mMesh->getNumSubEntities();++i) {
                auto sub=mMesh->getSubEntity(i); sub->setMaterial(meshPreviewMaterial(sub,mMesh->getMesh()));
            }
            if(tag == "npc" && mMesh->getAllAnimationStates()) {
                for(const char* animation : {"Idle", "idle", "Walk", "walk"})
                    if(mMesh->getAllAnimationStates()->hasAnimationState(animation)) { mMesh->getAnimationState(animation)->setEnabled(true); break; }
            }
        }
        if(tag == "light") {
            mLight = manager->createLight(getName()+"/light");
            const auto type = attr(e,"type","point");
            mLight->setType(type=="spot" ? Ogre::Light::LT_SPOTLIGHT : (type=="directional" ? Ogre::Light::LT_DIRECTIONAL : Ogre::Light::LT_POINT));
            mLight->setPowerScale(number(e,"power",1));
            if(auto color=e.FirstChildElement("colourDiffuse")) mLight->setDiffuseColour(number(*color,"r",1),number(*color,"g",1),number(*color,"b",1));
            if(auto a=e.FirstChildElement("lightAttenuation")) mLight->setAttenuation(number(*a,"range",2000),number(*a,"constant",1),number(*a,"linear"),number(*a,"quadratic"));
            if(auto r=e.FirstChildElement("lightRange")) if(type=="spot") mLight->setSpotlightRange(Ogre::Degree(number(*r,"inner",30)),Ogre::Degree(number(*r,"outer",60)));
            mLight->setCastShadows(false); mHandle->attachObject(mLight);
        }
        if(tag == "particleSystem" || tag == "psys" || tag == "fire") {
            const auto templ=attr(e,"template",attr(e,"templateName",attr(e,"particle")));
            if(!templ.empty()) {
                mParticles=manager->createParticleSystem(getName()+"/particle",templ); mHandle->attachObject(mParticles);
                auto mat=Ogre::MaterialManager::getSingleton().getByName(mParticles->getMaterialName(),PROJECT_RESOURCE_GROUP);
                if(auto preview=previewMaterial(mat)) mParticles->setMaterialName(preview->getName(),PROJECT_RESOURCE_GROUP);
            }
        }
    } catch(const Ogre::Exception& error) {
        Ogre::LogManager::getSingleton().logMessage("Run3 preview '"+getName()+"': "+error.getDescription());
    }
    const bool spatial=e.FirstChildElement("position") || e.Attribute("x") || e.Attribute("x1") || e.Attribute("pos");
    const bool volume=(tag=="trigger" || tag=="darkzone" || tag=="ladder") && e.FirstChildElement("scale");
    // DotScene transform nodes are containers, often scaled by 100. Drawing a
    // second marker for each would cover their child models with enormous lines.
    const bool helper=tag!="node" && (spatial || tag=="light" || tag=="timer" || tag=="event" || tag=="lua" || tag=="cutscene" || tag=="trigger" || tag=="sound");
    if(volume && !mMesh) {
        // Triangle-backed wire volumes remain pickable by Ogitor's existing
        // mesh raycaster, unlike line-only ManualObjects.
        const Ogre::String boxName="Run3/EditorVolume.mesh",matName="Run3/EditorVolume";
        if(!Ogre::MeshManager::getSingleton().resourceExists(boxName,PROJECT_RESOURCE_GROUP)) {
            auto material=Ogre::MaterialManager::getSingleton().create(matName,PROJECT_RESOURCE_GROUP);
            auto pass=material->getTechnique(0)->getPass(0);
            pass->setLightingEnabled(false); pass->setPolygonMode(Ogre::PM_WIREFRAME); pass->setCullingMode(Ogre::CULL_NONE);
            pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
            auto box=manager->createManualObject(); box->begin(material,Ogre::RenderOperation::OT_TRIANGLE_LIST);
            for(int i=0;i<8;++i) { box->position(i&1?0.5f:-0.5f,i&2?0.5f:-0.5f,i&4?0.5f:-0.5f); box->colour(1,0.5f,0.1f); }
            for(unsigned int i : {0,2,3,0,3,1,4,5,7,4,7,6,0,1,5,0,5,4,2,6,7,2,7,3,0,4,6,0,6,2,1,3,7,1,7,5}) box->index(i);
            box->end(); box->convertToMesh(boxName,PROJECT_RESOURCE_GROUP); manager->destroyManualObject(box);
        }
        mMesh=manager->createEntity(getName(),boxName,PROJECT_RESOURCE_GROUP);
        mMesh->setCastShadows(false); mMesh->setQueryFlags(QUERYFLAG_MOVABLE); mHandle->attachObject(mMesh);
        return;
    }
    if(!mMesh && helper && !volume) {
        mMesh=manager->createEntity(getName(),"scbMarker.mesh","EditorResources");
        mMesh->setCastShadows(false); mMesh->setQueryFlags(QUERYFLAG_MOVABLE); mHandle->attachObject(mMesh);
    }
    if(!helper || (!mesh.empty() && mMesh)) return;
    Ogre::ColourValue colour(0.4f,0.8f,1);
    if(tag=="trigger" || tag=="darkzone" || tag=="ladder") colour={1,0.5f,0.1f};
    else if(tag=="sound" || tag=="ambient") colour={0.2f,1,0.4f};
    else if(tag=="timer" || tag=="event" || tag=="lua") colour={1,0.3f,1};
    auto& mm=Ogre::MaterialManager::getSingleton();
    auto material=mm.getByName("Run3/EditorHelper",PROJECT_RESOURCE_GROUP);
    if(!material) { material=mm.create("Run3/EditorHelper",PROJECT_RESOURCE_GROUP); auto pass=material->getTechnique(0)->getPass(0); pass->setLightingEnabled(false); pass->setVertexColourTracking(Ogre::TVC_DIFFUSE); }
    mLines=manager->createManualObject(getName()+"/helper"); mLines->setQueryFlags(0); mLines->setCastShadows(false); mHandle->attachObject(mLines);
    mLines->begin(material,Ogre::RenderOperation::OT_LINE_LIST);
    auto line=[&](Ogre::Vector3 a,Ogre::Vector3 b) { mLines->position(a); mLines->colour(colour); mLines->position(b); mLines->colour(colour); };
    const float size=volume ? 0.5f : 32;
    line({-size,0,0},{size,0,0}); line({0,-size,0},{0,size,0}); line({0,0,-size},{0,0,size});
    if(volume || (e.Attribute("x1") && e.Attribute("x2"))) {
        auto min=volume ? Ogre::Vector3(-0.5f) : vector(e,"x1","y1","z1")-getPosition();
        auto max=volume ? Ogre::Vector3(0.5f) : vector(e,"x2","y2","z2")-getPosition();
        for(int i=0;i<8;++i) for(int bit=1;bit<=4;bit*=2) if(!(i&bit)) {
            auto point=[&](int mask) { return Ogre::Vector3(mask&1?max.x:min.x,mask&2?max.y:min.y,mask&4?max.z:min.z); };
            line(point(i),point(i|bit));
        }
    }
    if(tag=="sound" || tag=="light" || tag=="fire") {
        float radius=number(e,"radius",100); if(radius<=0) radius=100;
        for(int i=0;i<32;++i) { float a=i*Ogre::Math::TWO_PI/32,b=(i+1)*Ogre::Math::TWO_PI/32; line({radius*std::cos(a),0,radius*std::sin(a)},{radius*std::cos(b),0,radius*std::sin(b)}); }
    }
    mLines->end();
}
EntityFactory::EntityFactory(OgitorsView* view) : CNodeEditorFactory(view) {
    mTypeName="Run3 Entity"; mEditorType=ETYPE_MOVABLE; mIcon="node.svg";
    mCapabilities=CAN_MOVE|CAN_ROTATE|CAN_SCALE|CAN_FOCUS|CAN_DELETE|CAN_UNDO|CAN_CLONE|CAN_ACCEPTCOPY|CAN_ACCEPTPASTE;
    mAddToObjectList=false; mRequirePlacement=false; mUsesGizmos=true;
    AddPropertyDefinition("run3::xml","Run3 XML","Native XML template",PROP_STRING,false,false);
    AddPropertyDefinition("run3::slot","Run3 slot","Original XML ordering",PROP_INT,false,false);
    mPropertyDefs.at("orientation").setAccess(true,true);
    mPropertyDefs.at("updatescript").setAccess(false,false);
}
CBaseEditorFactory* EntityFactory::duplicate(OgitorsView* view) { auto f=OGRE_NEW EntityFactory(view); f->mTypeID=mTypeID; return f; }
CBaseEditor* EntityFactory::CreateObject(CBaseEditor** parent, OgitorsPropertyValueMap& params) {
    if(params.erase("init")) {
        const auto name=params.find("name");
        const Ogre::String label=name==params.end()?"Run3 Entity":Ogre::any_cast<Ogre::String>(name->second.val);
        params["name"]=OgitorsPropertyValue(PROP_STRING,Ogre::Any(CreateUniqueID(label)));
        params["run3::slot"]=OgitorsPropertyValue(PROP_INT,Ogre::Any(-1));
    }
    auto object=OGRE_NEW Entity(this); object->createProperties(params); object->mParentEditor->init(*parent); ++mInstanceCount; return object;
}
}
