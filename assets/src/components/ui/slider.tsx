import { forwardRef, type InputHTMLAttributes } from "react";
import { cn } from "../../lib/utils";

const Slider = forwardRef<
  HTMLInputElement,
  InputHTMLAttributes<HTMLInputElement>
>(({ className, ...props }, ref) => (
  <input
    type="range"
    className={cn(
      "h-4 w-full cursor-pointer accent-neutral-900 focus-visible:outline-none disabled:cursor-not-allowed disabled:opacity-50",
      className
    )}
    ref={ref}
    {...props}
  />
));
Slider.displayName = "Slider";

export { Slider };
