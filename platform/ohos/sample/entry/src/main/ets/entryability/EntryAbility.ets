import { UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

const LOG_DOMAIN: number = 0x4d4c4e;
const LOG_TAG: string = 'MapLibreSample';

export default class EntryAbility extends UIAbility {
  private lifecycleOnly: boolean = false;
  onCreate(want: Want): void {
    this.lifecycleOnly = want.parameters?.lifecycleOnly === true;
  }
  onWindowStageCreate(windowStage: window.WindowStage): void {
    const storage = new LocalStorage();
    storage.setOrCreate('lifecycleOnly', this.lifecycleOnly);
    windowStage.loadContent('pages/Index', storage, (error) => {
      if (error.code) {
        hilog.error(LOG_DOMAIN, LOG_TAG, 'Could not load page: %{public}s', JSON.stringify(error));
      }
    });
  }
}
