import { useLayoutEffect, useRef, useState, type CSSProperties } from "react";
import type { MediaState } from "./api";
import { songOffsetKey, wordFill, type LyricWord } from "./lyrics/lrc";
import { lyricClockAt } from "./lyrics/lyricClock";
import { useLyricClock } from "./lyrics/useLyricClock";
import { useLyrics, type LyricsSettings } from "./lyrics/useLyrics";
import { useSongOffset } from "./lyrics/useSongOffset";
import "./CollapsedLyric.css";

/** Snapshot the lyric clock on mount; CSS fills every frame without re-rendering the island. */
function CollapsedSungWords({ words, mediaState, offsetMs, playing }: {
  words: LyricWord[]; mediaState: MediaState; offsetMs: number; playing: boolean;
}) {
  const [start] = useState(() => lyricClockAt(null, mediaState, Date.now(), offsetMs).position);
  return words.map((word, index) => {
    const { delayMs, durationMs } = wordFill(word, start);
    const style = {
      animationDelay: `${delayMs}ms`, animationDuration: `${durationMs}ms`,
      animationPlayState: playing ? "running" : "paused",
    } as CSSProperties;
    return <span className="collapsed-lyric__word" key={index} style={style}>{word.text}</span>;
  });
}

/**
 * How far the line overflows its capsule, measured once it is laid out. The island resizes as the
 * text changes, so this is re-measured for every new line (and on a font or scale change).
 */
function useOverflow(active: boolean) {
  const boxRef = useRef<HTMLSpanElement | null>(null);
  const textRef = useRef<HTMLSpanElement | null>(null);
  const [overflow, setOverflow] = useState(0);

  useLayoutEffect(() => {
    const box = boxRef.current, text = textRef.current;
    if (!box || !text) return;
    const measure = () => {
      const excess = Math.max(0, Math.ceil(text.scrollWidth - box.clientWidth));
      setOverflow(excess);
    };
    measure();
    // The island's own width animates; measure again once it settles.
    const observer = new ResizeObserver(measure);
    observer.observe(box);
    return () => observer.disconnect();
  }, [active]);

  return { boxRef, textRef, overflow };
}

/**
 * The line being sung, for the collapsed island; the song's title and artist when it has no
 * lyrics or between lines. It wakes on its own for each line, so only this leaf re-renders while a
 * song plays; a new line replaces the last at once (the music page lets it rise in).
 * Keep prefetching while expanded; only the visible leaf drives the line-change clock.
 * A line wider than the capsule scrolls its tail into view rather than losing it to an ellipsis.
 */
export function CollapsedLyric({
  mediaState,
  lyricsSettings,
  active,
}: {
  mediaState: MediaState;
  lyricsSettings: LyricsSettings & { lyricsOffsetMs: number };
  active: boolean;
}) {
  const { lines } = useLyrics(mediaState, lyricsSettings);
  const [songOffsetMs] = useSongOffset(lines ? songOffsetKey(mediaState) : null);
  const offsetMs = lyricsSettings.lyricsOffsetMs + songOffsetMs;
  const { index, playing } = useLyricClock(lines, mediaState, offsetMs, active);
  const song = [mediaState.title, mediaState.artist].filter(Boolean).join(" · ");
  const line = lines && index >= 0 ? lines[index] : null;
  const words = lyricsSettings.lyricWordFill === true && line?.text ? line.words : undefined;
  const plain = line?.text || song;
  const { boxRef, textRef, overflow } = useOverflow(active);

  // Traversal time grows with the distance, so a long line is not a blur and a short one is not
  // a crawl; clamped to a range that reads comfortably.
  const scrollMs = Math.min(18000, Math.max(6000, Math.round(overflow * 22)));
  const style = overflow > 0
    ? ({ "--lyric-overflow": `${overflow}px`, "--lyric-scroll-ms": `${scrollMs}ms` } as CSSProperties)
    : undefined;

  return (
    <span
      className={`collapsed-lyric${overflow > 0 && !words ? " collapsed-lyric--scrolls" : ""}`}
      style={style}
      ref={boxRef}
    >
      <span className="collapsed-lyric__text" ref={textRef}>
        {words ? (
          <CollapsedSungWords
            key={`${index}:${mediaState.updatedAt}:${offsetMs}:${active}:${playing}`}
            words={words} mediaState={mediaState} offsetMs={offsetMs} playing={playing && active}
          />
        ) : plain}
      </span>
    </span>
  );
}
