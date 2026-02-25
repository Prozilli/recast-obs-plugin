#pragma once

#include <QHash>
#include <QObject>
#include <QString>

extern "C" {
#include "recast-output.h"
}

class RecastScenesDock;
class RecastSourcesDock;
class RecastPreviewDock;
class RecastSettingsDock;
class RecastDock;

/* Holds the 4 docks for one output */
struct RecastDockSet {
	RecastScenesDock *scenes;
	RecastSourcesDock *sources;
	RecastPreviewDock *preview;
	RecastSettingsDock *settings;
};

class RecastDockManager : public QObject {
	Q_OBJECT

public:
	explicit RecastDockManager(RecastDock *parent_dock);
	~RecastDockManager();

	/* Create 4 docks for an output and register them with OBS */
	void createDocksForOutput(recast_output_target_t *target);

	/* Destroy 4 docks for an output and unregister from OBS */
	void destroyDocksForOutput(recast_output_target_t *target);

	/* Destroy all managed docks */
	void destroyAll();

signals:
	/* Forwarded to the main dock for config save */
	void configChanged();

private:
	RecastDock *parent_dock_;
	QHash<QString, RecastDockSet *> docks_;
};
