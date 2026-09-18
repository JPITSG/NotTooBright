import { useState } from "react";
import { type ConfigData, saveSettings, closeDialog } from "./lib/bridge";
import { Button } from "./components/ui/button";
import { Checkbox } from "./components/ui/checkbox";
import { Label } from "./components/ui/label";

interface Props {
  config: ConfigData;
  webView2Version: string;
}

export default function ConfigView({ config, webView2Version }: Props) {
  const [startWithWindows, setStartWithWindows] = useState(
    config.startWithWindows ?? false
  );
  const [debugLog, setDebugLog] = useState(config.debugLog ?? false);

  function handleSave() {
    saveSettings({
      startWithWindows,
      debugLog,
    });
  }

  return (
    <div className="p-4 space-y-3">
      <div className="space-y-3">
        <div className="flex items-start gap-2 pt-1">
          <Checkbox
            id="startWithWindows"
            className="mt-0.5"
            checked={startWithWindows}
            onChange={(e) => setStartWithWindows(e.target.checked)}
          />
          <div className="space-y-0.5">
            <Label htmlFor="startWithWindows" className="cursor-pointer">
              Start NotTooBright when you sign in
            </Label>
            <p className="text-neutral-500 text-[11px] leading-snug">
              Adds the application to your account's startup programs so it
              is in the system tray after every sign-in. Only affects the
              current Windows user.
            </p>
          </div>
        </div>

        <div className="flex items-start gap-2 pt-1">
          <Checkbox
            id="debugLog"
            className="mt-0.5"
            checked={debugLog}
            onChange={(e) => setDebugLog(e.target.checked)}
          />
          <div className="space-y-0.5">
            <Label htmlFor="debugLog" className="cursor-pointer">
              Enable debug logging
            </Label>
            <p className="text-neutral-500 text-[11px] leading-snug">
              Writes diagnostic events to{" "}
              %LOCALAPPDATA%\NotTooBright\debug.log. Useful when reporting
              issues; leave off for normal use.
            </p>
          </div>
        </div>
      </div>

      <div className="flex items-center justify-between gap-3 pt-1">
        <span
          className="select-none whitespace-nowrap text-[11px] leading-none tabular-nums text-neutral-400"
          title="Application version / WebView2 version"
        >
          v{__APP_VERSION__} / {webView2Version}
        </span>
        <div className="flex items-center gap-2">
          <Button
            variant="outline"
            size="sm"
            className="min-w-[5rem]"
            onClick={closeDialog}
          >
            Cancel
          </Button>
          <Button size="sm" className="min-w-[5rem]" onClick={handleSave}>
            Save
          </Button>
        </div>
      </div>
    </div>
  );
}
