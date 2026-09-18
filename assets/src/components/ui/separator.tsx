import { cn } from "../../lib/utils";

function Separator({ className }: { className?: string }) {
  return (
    <div
      className={cn("shrink-0 bg-neutral-200 h-[1px] w-full", className)}
    />
  );
}

export { Separator };
