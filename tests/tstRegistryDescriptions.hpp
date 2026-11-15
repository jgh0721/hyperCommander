#pragma once
#include <QObject>
class RegistryDescriptionTests final : public QObject
{
    Q_OBJECT
private slots:
    void asynchronousReload();
    void builtinCatalog();
    void policyPrograms();
    void policyObservation();
    void policyEditPlan();
    void policySoftAndAdditive();
    void xmlLimits();
    void namespacesTranslationsAndCache();
    void systemCatalogReadOnly();
};
