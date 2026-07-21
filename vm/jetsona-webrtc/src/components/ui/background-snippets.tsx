// Decorative full-bleed background layers.
//
// Both are pure-CSS, absolutely positioned and non-interactive, so they can be
// dropped behind any shell without affecting layout or hit-testing. `fixed` (not
// `absolute`) keeps them pinned while the console scales in and out of
// fullscreen.

export const GridBackground = () => {
  return (
    <div className="pointer-events-none fixed inset-0 -z-10 h-full w-full bg-white bg-[linear-gradient(to_right,#f0f0f0_1px,transparent_1px),linear-gradient(to_bottom,#f0f0f0_1px,transparent_1px)] bg-[size:6rem_4rem]">
      <div className="absolute bottom-0 left-0 right-0 top-0 bg-[radial-gradient(circle_800px_at_100%_200px,#d5c5ff,transparent)]" />
    </div>
  );
};

export const RadialGlowBackground = () => {
  return (
    <div className="pointer-events-none fixed inset-0 -z-10 h-full w-full [background:radial-gradient(125%_125%_at_50%_10%,#000_40%,#63e_100%)]" />
  );
};

// The console runs on a dark surface, so the radial glow is the default export.
export const Component = RadialGlowBackground;
