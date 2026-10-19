#pragma once
#include "Ogitors.h"
#include "Run3Document.h"

namespace Run3Editor {
class EntityFactory;
class Entity : public Ogitors::CNodeEditor, public Ogitors::OgitorsPropertySetListener {
    friend class EntityFactory;
public:
    void createProperties(Ogitors::OgitorsPropertyValueMap& params) override;
    bool load(bool async = true) override;
    bool unLoad() override;
    Ogre::AxisAlignedBox getAABB() override;
    std::unique_ptr<TiXmlElement> xml();
    static Entity* createTree(TiXmlElement&, Ogitors::CBaseEditor* parent, int slot, bool interactive = false);
    bool getObjectContextMenu(Ogitors::UTFStringVector&) override;
    void onObjectContextMenu(int selection) override;
    int slot() const { return mSlot->get(); }
    void OnPropertyChanged(Ogitors::OgitorsPropertySet*, Ogitors::OgitorsPropertyBase*) override;
    void OnPropertyAdded(Ogitors::OgitorsPropertySet* set, Ogitors::OgitorsPropertyBase* property) override { OnPropertyChanged(set,property); }
    void OnPropertyRemoved(Ogitors::OgitorsPropertySet* set, Ogitors::OgitorsPropertyBase* property) override { OnPropertyChanged(set,property); }
    void OnPropertySetRebuilt(Ogitors::OgitorsPropertySet*) override;
protected:
    Entity(Ogitors::CBaseEditorFactory* factory);
    ~Entity() override;
private:
    Ogitors::OgitorsProperty<Ogre::String>* mXml;
    Ogitors::OgitorsProperty<int>* mSlot;
    Ogre::Entity* mMesh = nullptr;
    Ogre::ManualObject* mLines = nullptr;
    Ogre::Light* mLight = nullptr;
    Ogre::ParticleSystem* mParticles = nullptr;
    Ogre::Vector3 mInitialPosition = Ogre::Vector3::ZERO, mInitialScale = Ogre::Vector3::UNIT_SCALE;
    Ogre::Quaternion mInitialOrientation = Ogre::Quaternion::IDENTITY;
    bool mRefreshing = false;
    void readTransform(TiXmlElement& element);
    void clearPreview();
    void preview(TiXmlElement& element);
};
class EntityFactory : public Ogitors::CNodeEditorFactory {
public:
    EntityFactory(Ogitors::OgitorsView* view = nullptr);
    Ogitors::CBaseEditorFactory* duplicate(Ogitors::OgitorsView* view) override;
    Ogitors::CBaseEditor* CreateObject(Ogitors::CBaseEditor** parent, Ogitors::OgitorsPropertyValueMap& params) override;
};
}
