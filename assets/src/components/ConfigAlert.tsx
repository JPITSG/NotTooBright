import { useEffect, useRef, type ReactNode } from "react";

interface ConfigAlertProps {
  id: string;
  title: string;
  message: string;
  onEscape?: () => void;
  children: ReactNode;
}

export default function ConfigAlert({
  id, title, message, onEscape, children,
}: ConfigAlertProps) {
  const dialogRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const previousFocus = document.activeElement;
    const dialog = dialogRef.current;
    if (dialog && !dialog.contains(previousFocus)) {
      dialog.querySelector<HTMLElement>("button:not(:disabled)")?.focus();
    }
    return () => {
      if (previousFocus instanceof HTMLElement && previousFocus.isConnected) {
        previousFocus.focus();
      }
    };
  }, []);

  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      const dialog = dialogRef.current;
      if (!dialog) return;
      if (event.key === "Escape") {
        event.preventDefault();
        event.stopImmediatePropagation();
        onEscape?.();
      } else if (event.key === "Tab") {
        const controls = Array.from(dialog.querySelectorAll<HTMLElement>(
          'button:not(:disabled), input:not(:disabled), [tabindex="0"]'
        ));
        const first = controls[0];
        const last = controls[controls.length - 1];
        if (!first) {
          event.preventDefault();
        } else if (!dialog.contains(document.activeElement)) {
          event.preventDefault();
          (event.shiftKey ? last : first).focus();
        } else if (event.shiftKey && document.activeElement === first) {
          event.preventDefault();
          last.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault();
          first.focus();
        }
      }
    };
    document.addEventListener("keydown", handleKeyDown);
    return () => document.removeEventListener("keydown", handleKeyDown);
  }, [onEscape]);

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/35 p-4"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) event.preventDefault();
      }}
    >
      <div
        ref={dialogRef}
        role="alertdialog"
        aria-modal="true"
        aria-labelledby={`${id}-title`}
        aria-describedby={`${id}-message`}
        className="w-full max-w-sm space-y-3 rounded-lg border border-neutral-200 bg-white p-4 shadow-xl"
      >
        <div className="space-y-1">
          <h2 id={`${id}-title`} className="text-sm font-semibold">{title}</h2>
          <p id={`${id}-message`} className="text-xs leading-relaxed text-neutral-600">
            {message}
          </p>
        </div>
        {children}
      </div>
    </div>
  );
}
