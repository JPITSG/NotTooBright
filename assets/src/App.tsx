import { useEffect, useRef, useState } from "react";
import {
  type InitData,
  type MonitorData,
  onInit,
  onMonitors,
  onRemoteSession,
  getInit,
  reportSize,
} from "./lib/bridge";
import ConfigView from "./ConfigView";

export default function App() {
  const [initData, setInitData] = useState<InitData | null>(null);
  const [monitors, setMonitors] = useState<MonitorData[]>([]);
  const [remoteSession, setRemoteSession] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    onInit((data) => {
      setMonitors(data.monitors ?? []);
      setRemoteSession(data.config?.remoteSession ?? false);
      setInitData(data);
    });
    const removeMonitorsListener = onMonitors((list) => setMonitors(list));
    const removeRemoteListener = onRemoteSession((remote) => setRemoteSession(remote));
    getInit();
    return () => {
      removeMonitorsListener();
      removeRemoteListener();
    };
  }, []);

  useEffect(() => {
    const el = rootRef.current;
    if (!el || !initData) return;

    const report = () => reportSize(Math.ceil(el.scrollHeight));
    const rafId = requestAnimationFrame(report);

    const observer = new ResizeObserver(report);
    observer.observe(el);
    return () => {
      cancelAnimationFrame(rafId);
      observer.disconnect();
    };
  }, [initData]);

  if (!initData) return null;

  return (
    <div ref={rootRef}>
      <ConfigView
        config={initData.config}
        monitors={monitors}
        remoteSession={remoteSession}
        updateCompletedVersion={initData.updateCompletedVersion ?? ""}
      />
    </div>
  );
}
