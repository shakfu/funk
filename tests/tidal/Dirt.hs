{-# LANGUAGE OverloadedStrings #-}
-- The cases of tests/tidal/dirt.cases, as Tidal 1.10.3's superdirtTarget
-- sends them: each onset of cycles [0, 4) at 0.5 cps through Tidal's own
-- toOSC, as d1 plays it (Boot.hs: p 1 . (|< orbit 0)), a float printed as its bits. An event's times are
-- in microseconds, to the nearest, as Link's are (assumed: Link's llround),
-- and delta is their difference.
-- tests/tidal/dirt.sh diffs this against funk-midi --dirt.
import Data.Int (Int64)
import Data.List (sortOn)
import Control.Monad (foldM_)
import qualified Data.Map as Map
import qualified Sound.Osc.Fd as O
import GHC.Float (castFloatToWord32)
import Sound.Tidal.Context
import Sound.Tidal.Stream.Process (ProcessedEvent (..), playStack, toOSC)
import Sound.Tidal.Stream.Types (PlayState (..))
import Sound.Tidal.Stream.Target (superdirtShape)
import Text.Printf (printf)

datum :: O.Datum -> String
datum (O.Int32 i) = "i:" ++ show i
datum (O.Float f) = printf "f:%08x" (castFloatToWord32 f)
datum (O.AsciiString s) = "s:" ++ O.ascii_to_string s
datum (O.Blob b) = "b:" ++ concatMap (printf "%02x") (O.blob_unpack b)
datum d = show d

pairs :: [O.Datum] -> String
pairs (k : v : rest) = " " ++ O.ascii_to_string (O.d_ascii_string k) ++ "=" ++ datum v ++ pairs rest
pairs _ = ""

-- a cycle's time in microseconds at 0.5 cps, to the nearest
us :: Rational -> Int64
us c = floor (c * 2000000 + 1 / 2)

out :: String -> ControlPattern -> IO ()
out name pat = outStack name (slots [("1", pat |< orbit 0)])

-- patterns by ID, as streamReplace keeps them, through Tidal's playStack
slots :: [(String, ControlPattern)] -> ControlPattern
slots kps = slotsWith [(k, pat, False, False) | (k, pat) <- kps]

-- with each slot's psMute and psSolo
slotsWith :: [(String, ControlPattern, Bool, Bool)] -> ControlPattern
slotsWith kps = playStack $ Map.fromList [(k, PlayState (pat # pS "_id_" (pure k)) m so []) | (k, pat, m, so) <- kps]

outStack :: String -> ControlPattern -> IO ()
outStack name pat = mapM_ line es
  where
    -- funk-midi's windows, 10 ms at 0.5 cps: a signal joined in, as by
    -- fadeOutFrom, is read once a window, as Tidal reads it once a frame
    es = sortOn (start . part) $ concat [filter eventHasOnset $ queryArc pat (Arc (k / 200) ((k + 1) / 200)) | k <- [0 .. 799]]
    pe e = ProcessedEvent
      { peHasOnset = True, peEvent = e, peCps = 0.5,
        peDelta = us (stop (wholeOrPart e)) - us (start (wholeOrPart e)),
        peCycle = start (wholeOrPart e), peOnWholeOrPart = 0, peOnWholeOrPartOsc = 0, peOnPart = 0, peOnPartOsc = 0 }
    line e = mapM_ (\(_, _, O.Message p ds) -> putStrLn (name ++ " " ++ p ++ pairs ds)) (toOSC Nothing (pe e) superdirtShape)

-- as outStack, with Tidal's state: each window's events sorted by part and
-- passed through resolveState, its map kept for the next, as doTick does
outStateStack :: String -> ControlPattern -> IO ()
outStateStack name pat = foldM_ step Map.empty [0 .. 799 :: Integer]
  where
    step sm k = do
      let es = sortOn (start . part) $ query pat (State (Arc (fromIntegral k / 200) (fromIntegral (k + 1) / 200)) sm)
          (sm', es') = resolveState sm es
      mapM_ line (filter eventHasOnset es')
      return sm'
    pe e = ProcessedEvent
      { peHasOnset = True, peEvent = e, peCps = 0.5,
        peDelta = us (stop (wholeOrPart e)) - us (start (wholeOrPart e)),
        peCycle = start (wholeOrPart e), peOnWholeOrPart = 0, peOnWholeOrPartOsc = 0, peOnPart = 0, peOnPartOsc = 0 }
    line e = mapM_ (\(_, _, O.Message p ds) -> putStrLn (name ++ " " ++ p ++ pairs ds)) (toOSC Nothing (pe e) superdirtShape)

main :: IO ()
main = do
  out "basic" (s "bd*2 [sn cp]" # n "<0 3>")
  out "kinds" (s "x" # cut "1 2" # vowel "a" # orbit 1)
  out "sound" (sound "bd:3 sn:1 [hh hh:2]")
  out "notes" (s "superpiano" # note "c e g a5")
  out "floats" (s "x*4" # pan sine # gain 1.2 # speed "-1" # legato 0.5)
  out "nos" (n "0 1")
  out "stack" (stack [s "a*2", s "b" # n 1])
  out "own" (s "x" # pF "delta" 0.5 # pF "cycle" 9 # pF "cps" 0.5)
  out "ownid" (s "x" # pS "_id_" "7")
  out "slow" (slow 2 $ s "long" # n "<1 2>")
  out "every" (every 2 (fast 2) $ s "bd sn" # crush 4)
  out "cont" (s "x*8" # cutoff (range 200 2000 saw) # resonance perlin)
  -- d1, d10, d2 and d13 as Boot.hs defines them, and p "x"
  outStack "slots" (slots [("1", s "a*2" |< orbit 0), ("10", s "b" |< orbit 9), ("2", s "c [d e]" |< orbit 1),
                           ("x", s "f*3" # orbit 5), ("13", s "<g h>")])
  -- mute 2, then solo 1 and x: the solos play, muted or not
  outStack "muted" (slotsWith [("1", s "a" |< orbit 0, False, False), ("2", s "b*2" |< orbit 1, True, False), ("x", s "c", False, False)])
  outStack "soloed" (slotsWith [("1", s "a" |< orbit 0, False, True), ("2", s "b*2" |< orbit 1, True, False), ("x", s "c", True, True)])
  -- transitions at cycle 0 (Transition.hs): d1 old, then one, as the slot then plays mapper 0 history
  let d1st pat = (pat |< orbit 0) # pS "_id_" (pure "1")
      a = s "a*4" # n "0 1"
  outStack "xfade" (_xfadeIn 4 0 [s "b*4", d1st a])
  outStack "xfadeIn" (_xfadeIn 2 0 [s "b*4" # gain 0.9, d1st a])
  outStack "jumpIn" (_jumpIn 2 0 [s "b*2", d1st a])
  outStack "jumpIn'" (_jumpIn' 1 0 [s "b*2", d1st a])
  outStack "jumpMod" (_jumpMod 3 0 [s "b*2", d1st a])
  outStack "wait" (_wait 2 0 [s "b*2", d1st a])
  outStack "waitT" (_waitT (_xfadeIn 1) 1 0 [s "b*2", d1st a])
  outStack "histpan" (_histpan 3 0 [s "c", s "b" |< orbit 0, d1st a])
  outStack "mortal" (_mortal 1 2 0 [s "b*8", d1st a])
  outStack "interpolate" (_interpolateIn 2 0 [s "x*4" # speed 2 # pan 0.5 # cut 4, d1st (s "x*4" # speed 1 # cut 1)])
  outStack "clutch" (_clutchIn 2 0 [s "b*8", d1st (s "a*8")])
  outStack "forId" (_mortalOverlay 2 0 [s "b*2", d1st a])
  -- state: counters and lists, shared across slots (C3)
  outStateStack "count" (slots [("1", s "a*4" # nCount "c" # speedTake "t" [1, 2, 3] |< orbit 0), ("2", s "b*2" |+ nCount "c" |< orbit 1)])
  outStateStack "countTo" (slots [("1", s "a*8" # nCountTo "k" "<3 5>" |< orbit 0)])
  outStateStack "takeS" (slots [("1", s "a*3" # sTake "v" ["a", "e"] # cutCount "q" |+ cut 10 |< orbit 0)])
  outStateStack "bools" (slots [("1", s "a" # pB "flag" "t f" # pR "r" "0.25" # array (pure [1, 2, 255]) |< orbit 0)])
