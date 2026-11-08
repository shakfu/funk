-- The cases of tests/tidal/compare.fk, queried in Tidal (tidal-core 1.10.2):
-- each prints the onsets of its first cycles, in ticks, with values to 1e-9.
-- tests/tidal/compare.sh diffs the two outputs.
import Sound.Tidal.Context hiding (s, n)
import qualified Sound.Tidal.Context as T
import Data.List (sortOn, sort)
import qualified Data.Map as Map
import Data.String (fromString)

tk :: Rational -> Integer
tk r = floor (r * 720720)

fmt :: Double -> String
fmt x = show (round (x * 1e9) :: Integer)

-- the onsets of cycles [0, k), each cycle sorted
cycOn :: Int -> (a -> String) -> Pattern a -> [(Integer, Integer, String)]
cycOn k r p = concatMap one [0 .. k - 1]
  where
    one c = sort $ map (\e -> let Arc a b = wholeOrPart e in (tk a, tk b, r (value e)))
              $ filter eventHasOnset $ queryArc p (Arc (fromIntegral c) (fromIntegral c + 1))

-- cycOn, each query with controls m
cycOnC :: Int -> (a -> String) -> ValueMap -> Pattern a -> [(Integer, Integer, String)]
cycOnC k r m p = concatMap one [0 .. k - 1]
  where
    one c = sort $ map (\e -> let Arc a b = wholeOrPart e in (tk a, tk b, r (value e)))
              $ filter eventHasOnset $ query p (State (Arc (fromIntegral c) (fromIntegral c + 1)) m)

outC :: String -> Int -> (a -> String) -> [(String, Value)] -> Pattern a -> IO ()
outC name k r m p = putStrLn (name ++ " " ++ show (cycOnC k r (Map.fromList m) p))

out :: String -> Int -> (a -> String) -> Pattern a -> IO ()
out name k r p = putStrLn (name ++ " " ++ show (cycOn k r p))

-- values of a continuous pattern at the starts of quarter cycles
sampled :: String -> Pattern Double -> IO ()
sampled name p = putStrLn (name ++ " " ++ show [concatMap (fmt . value) (queryArc p (Arc t t)) | t <- [0, 1/4 .. 7/4]])

rI :: Int -> String
rI = show
rF :: Double -> String
rF = fmt
rS :: String -> String
rS = id
rB :: Bool -> String
rB b = if b then "t" else "f"

rV :: Value -> String
rV v = case v of
  VF x -> "f" ++ fmt x
  VN (Note x) -> "f" ++ fmt x
  VI i -> "i" ++ show i
  VS x -> "s" ++ x
  VR x -> "f" ++ fmt (fromRational x)
  _ -> "?"

rC :: ValueMap -> String
rC m = concatMap (\(k, v) -> k ++ "=" ++ rV v ++ ";") (Map.toList m)

nums :: Pattern Int
nums = fastFromList [0, 1, 2, 3]
abcd :: Pattern String
abcd = fastFromList ["a", "b", "c", "d"]
chord :: Pattern Int
chord = stack [pure 0, pure 4, pure 7]
tf :: Pattern Bool
tf = fastFromList [True, False, True, True]
bdsn :: ControlPattern
bdsn = T.s (fastFromList ["bd", "sn"])

main :: IO ()
main = do
  sampled "sine" sine
  sampled "cosine" cosine
  sampled "saw" saw
  sampled "isaw" isaw
  sampled "tri" tri
  sampled "square" square
  sampled "envL" (_slow 2 envL)
  sampled "envEq" (_slow 2 envEq)
  sampled "rand" rand
  sampled "perlin" (_slow 2 perlin)
  sampled "perlin2" (perlin2 (sine * 4))
  sampled "normal" normal
  out "segsaw" 1 rF (segment 4 saw)
  out "segrange" 1 rF (segment 4 (range 1 5 sine))
  out "irand" 8 rI (segment 1 (irand 8))
  out "choose" 8 rS (segment 1 (choose ["a", "b", "c"]))
  out "wchoose" 8 rS (segment 1 (wchoose [("a", 1), ("b", 2), ("c", 0.5)]))
  out "brand" 2 rB (segment 4 brand)
  out "degrade" 4 rI (degrade (_fast 8 (pure 1)))
  out "degradeBy" 4 rI (degradeBy 0.3 (_fast 8 nums))
  out "unDegradeBy" 4 rI (unDegradeBy 0.3 (_fast 8 nums))
  out "sometimesBy" 4 rI (sometimesBy 0.5 (fast 2) nums)
  out "sometimesBy'" 4 rI (sometimesBy' 0.5 (+ 10) nums)
  out "someCyclesBy" 8 rI (someCyclesBy 0.5 (+ 10) nums)
  out "randcat" 8 rS (randcat [pure "a", pure "b", pure "c"])
  out "wrandcat" 8 rS (wrandcat [(pure "a", 1), (pure "b", 3)])
  out "shuffle" 4 rI (shuffle 4 nums)
  out "scramble" 4 rI (scramble 4 nums)
  out "randrun" 4 rI (randrun 5)
  out "rotR" 1 rS (0.25 ~> abcd)
  out "rotLpat" 1 rS (fastFromList [0, 0.25] <~ abcd)
  out "every" 4 rS (every 3 (fast 2) (fastFromList ["a", "b"]))
  out "every'" 5 rS (every' 3 1 (fast 2) (pure "a"))
  out "foldEvery" 6 rI (foldEvery [2, 3] (+ 10) nums)
  out "whenmod" 5 rS (whenmod 4 2 (fast 2) (fastFromList ["a", "b"]))
  out "zoom" 1 rS (zoom (0.25, 0.75) abcd)
  out "compress" 1 rS (compress (0.25, 0.75) (fastFromList ["a", "b"]))
  out "fastGap" 2 rS (fastGap 2 abcd)
  out "run" 1 rI (run 4)
  out "scan" 3 rI (scan 3)
  out "mono" 2 rS (mono (stack [pure "x", 0.5 ~> pure "y"]))
  out "iter" 4 rS (iter 4 abcd)
  out "iter'" 4 rS (iter' 4 abcd)
  out "palindrome" 2 rS (palindrome (fastFromList ["a", "b", "c"]))
  out "brak" 2 rS (brak (fastFromList ["a", "b"]))
  out "ply" 1 rS (ply 2 (fastFromList ["a", "b"]))
  out "plyWith" 1 rI (plyWith 3 (+ 10) (fastFromList [0, 1]))
  out "chunk" 4 rI (chunk 4 (+ 10) nums)
  out "chunk'" 4 rI (chunk' 4 (+ 10) nums)
  out "linger" 1 rS (linger 0.25 abcd)
  out "lingerneg" 1 rS (linger (-0.25) abcd)
  out "trunc" 1 rS (trunc 0.75 abcd)
  out "off" 1 rI (off 0.25 (+ 12) (pure 0))
  out "superimpose" 1 rI (superimpose (fast 2) (fastFromList [0, 1]))
  out "within" 1 rS (within (0, 0.5) (fast 2) abcd)
  out "within'" 1 rS (within' (0, 0.5) (fast 2) abcd)
  out "inside" 1 rS (inside 2 rev abcd)
  out "outside" 2 rS (outside 2 rev (cat [abcd, fastFromList ["e", "f"]]))
  out "euclid" 1 rS (euclid 3 8 (pure "x"))
  out "euclidneg" 1 rS (euclid (-3) 8 (pure "x"))
  out "euclidFull" 1 rS (euclidFull 3 8 (pure "x") (pure "."))
  out "euclidOff" 1 rS (euclidOff 3 8 2 (pure "x"))
  out "euclidOffBool" 1 rB (euclidOffBool 3 8 1 (pure True))
  out "distrib" 1 rS (distrib [3, 5] (pure "x"))
  out "wedge" 1 rS (wedge (1/4) (fastFromList ["a", "b"]) (fastFromList ["c", "d"]))
  out "arpup" 1 rI (arp (pure "up") chord)
  out "arpdown" 1 rI (arp (pure "down") chord)
  out "arpupdown" 1 rI (arp (pure "updown") chord)
  out "arpconverge" 1 rI (arp (pure "converge") (stack (map pure [1, 2, 3, 4, 5])))
  out "arppinky" 1 rI (arp (pure "pinkyup") (stack (map pure [1, 2, 3])))
  out "arpthumb" 1 rI (arp (pure "thumbupdown") (stack (map pure [1, 2, 3])))
  out "rolled" 1 rI (rolled chord)
  out "rolledneg" 1 rI (rolledBy (-0.25) chord)
  out "struct" 1 rS (struct tf (pure "a"))
  out "mask" 1 rS (mask tf abcd)
  out "stitch" 1 rS (stitch (fastFromList [True, False]) (pure "a") (pure "b"))
  out "sew" 1 rS (sew (fastFromList [True, False]) (fast 4 (pure "a")) (fast 3 (pure "b")))
  out "while" 1 rS (while (fastFromList [True, False]) (fast 2) abcd)
  out "ifp" 3 rI (ifp even (+ 10) (+ 20) (pure 0))
  out "segment" 1 rI (segment 3 (pure 1))
  out "range" 1 rF (range 1 5 (fastFromList [0, 0.5]))
  out "quantise" 1 rF (quantise 4 (fastFromList [0.3, 0.6]))
  out "rot" 1 rS (rot 1 abcd)
  out "rotneg" 1 rS (rot (-1) abcd)
  out "toScale" 1 rI (toScale [0, 2, 4, 5, 7, 9, 11] (fastFromList [0, 1, 2, 7, -1]))
  out "binary" 1 rB (binary 5)
  out "ascii" 1 rB (ascii (pure "a"))
  out "fit" 2 rS (fit 1 ["a", "b", "c"] (run 2))
  out "stutter" 1 rS (stutter (3 :: Int) 0.125 (pure "a"))
  out "swingBy" 1 rS (swingBy (1/3) 4 (fast 8 (pure "x")))
  out "bite" 1 rI (bite 4 (fastFromList [0, 2]) (fastFromList [0 .. 7]))
  out "squeeze" 1 rS (squeeze (fastFromList [0, 1]) [abcd, pure "z"])
  out "chromaticise" 1 rI (chromaticiseBy 2 (pure 60))
  out "press" 1 rS (press (fastFromList ["a", "b"]))
  out "smooth" 1 rF (segment 8 (smooth (fastFromList [0, 1])))
  out "spread" 3 rS (spread fast [1, 2] abcd)
  out "fastspread" 1 rS (fastspread fast [1, 2] abcd)
  out "select" 1 rS (select 0.6 [abcd, pure "z"])
  out "pickF" 2 rI (pickF (cat [0, 1]) [(+ 10), (+ 20)] nums)
  out "necklace" 2 rB (necklace 12 [4, 2])
  out "snowball" 4 rI (snowball 4 (+) (fast 2) (pure 1))
  out "soak" 3 rI (soak 3 (fast 2) (pure 1))
  out "markov" 1 rI (markovPat 8 1 [[0.5, 0.5], [0.3, 0.7]])
  out "beat" 1 rS (beat 1 4 (pure "x"))
  out "permstep" 1 rS (permstep 4 ["a", "b"] (pure 0))
  out "ur" 2 rS (ur 2 (fastFromList ["a", "b:fast"]) [("a", fastFromList ["x", "y"]), ("b", pure "z")] [("fast", fast 2)])
  out "stripe" 2 rS (stripe 2 (fastFromList ["a", "b"]))
  out "randStruct" 2 rI (randStruct 3)
  out "fit'" 2 rI (fit' 1 2 (fastFromList [0, 1]) (fastFromList [1, 0]) (fastFromList [0, 1]))
  out "inhabit" 1 rS (inhabit [("a", fastFromList ["x", "y"])] (fastFromList ["a", "b"]))
  out "spaceOut" 2 rS (spaceOut [1, 0.5] abcd)
  out "seqP" 3 rS (seqP [(0, 1, pure "a"), (1, 3, fast 2 (pure "b"))])
  out "index" 1 rS (index 0.25 (pure 1) abcd)
  out "chooseBy" 4 rS (segment 1 (chooseBy rand ["a", "b"]))
  out "tabby" 2 rS (tabby 2 (fastFromList ["a", "b"]) (fastFromList ["c", "d"]))
  out "rangex" 1 rF (rangex 20 2000 (fastFromList [0, 0.5]))
  out "fadeOut" 4 rI (fadeOut 4 (_fast 8 (pure 1)))
  out "degradeOverBy" 4 rI (degradeOverBy 2 0.5 (_fast 8 (pure 1)))
  putStrLn ("lindenmayer " ++ show (lindenmayer 3 "a:b,b:ab" "a"))
  putStrLn ("runMarkov " ++ show (runMarkov 8 [[0, 1], [1, 0]] 0 0))
  putStrLn ("deconstruct " ++ show (deconstruct 4 (fastFromList ["bd", "sn"])))
  out "chop" 1 rC (chop 2 bdsn)
  out "striate" 1 rC (striate 2 bdsn)
  out "striateBy" 1 rC (striateBy 2 0.5 bdsn)
  out "jux" 1 rC (jux rev bdsn)
  out "juxBy" 1 rC (juxBy 0.5 rev bdsn)
  out "juxcut" 1 rC (juxcut rev bdsn)
  out "jux4" 1 rC (jux4 rev bdsn)
  out "slice" 1 rC (slice 4 (fastFromList [0, 2]) (T.s (pure "bev")))
  out "splice" 1 rC (splice 4 (fastFromList [0, 2]) (T.s (pure "bev")))
  out "randslice" 4 rC (randslice 4 (T.s (pure "bev")))
  out "stut" 1 rC (stut 3 0.5 0.125 (T.s (pure "bd")))
  out "echo" 1 rC (echo 3 0.2 0.5 (T.s (pure "bd")))
  out "echoWith" 1 rC (echoWith 3 (1/8) (|* speed 2) (T.s (pure "bd")))
  out "fix" 1 rC (fix (fast 2) (T.s (pure "sn")) bdsn)
  out "loopAt" 2 rC (loopAt 2 (chop 4 (T.s (pure "break"))))
  out "hurry" 1 rC (hurry 2 bdsn)
  out "gap" 1 rC (gap 4 (T.s (pure "x")))
  out "ghost" 1 rC (ghost (T.s (pure "bd")))
  out "spin" 1 rC (spin 2 bdsn)
  out "chew" 1 rC (chew 4 (fastFromList [0, 1]) (T.n (fastFromList [0, 1, 2, 3]) # T.s (pure "x")))
  out "weave" 2 rC (weave 2 (pan sine) [T.s (pure "a"), T.s (pure "b")])
  out "stackwith" 1 rC (stackwith (pan (fastFromList [0, 1])) [T.s (pure "a"), T.s (pure "b")])
  out "splat" 1 rC (splat 2 (T.s (pure "a")) (T.s (pure "b")))
  out "smash" 3 rC (smash 2 [1, 2] (T.s (pure "a")))
  out "grain" 1 rC (grain 0.2 0.1)
  out "interlace" 1 rC (interlace (T.s (pure "a")) (T.s (pure "b")))
  -- a mini-notation step longer than a cycle: its whole is from its own cycle
  out "miniStep" 4 rS (spread' (slow . pure) (fastFromList [3, 4, 5, 2]) (fromString "[bd*5 ht*5, [sid/4]*3 [lt*2]/5 mt*3, [sid/3]*3]"))
  out "chunkInto" 4 rI (chunkInto 4 (+ 10) nums)
  out "chunkIntoneg" 4 rI (chunkInto (-4) (+ 10) nums)
  out "ribbon" 4 rS (ribbon 0.5 2 (cat [abcd, fastFromList ["e", "f"], pure "g"]))
  out "into" 2 rI (into (fastFromList [True, False]) (fast 2) nums)
  out "reset" 2 rS (reset (fastFromList [False, True]) abcd)
  out "restart" 2 rS (restart (fastFromList [False, True]) (slow 2 abcd))
  out "resetTo" 1 rS (resetTo (fastFromList [0, 0.25]) abcd)
  out "restartTo" 2 rS (restartTo (fastFromList [0, 0.25]) (slow 2 abcd))
  out "timeCat" 2 rS (timeCat [(1, fastFromList ["a", "b"]), (2, abcd)])
  out "markov2" 2 rI (markovPat 8 0 [[0.2, 0, 0.8], [0, 0, 1], [0.5, 0.5, 0]])
  putStrLn ("runMarkov2 " ++ show (runMarkov 12 [[0.2, 0, 0.8], [0, 0, 1], [0.5, 0.5, 0]] 0 3))
  outC "cF" 2 rF [("x", VF 0.25)] (cF 0.5 "x")
  outC "cFabsent" 1 rF [] (cF 0.5 "x")
  outC "cF_" 1 rF [] (cF_ "x")
  outC "cF0" 1 rF [] (cF0 "x")
  outC "cFint" 1 rF [("x", VI 3)] (cF 0 "x")
  outC "cI" 1 rI [("x", VF 2.7)] (cI 0 "x")
  outC "cS" 1 rS [("x", VS "hi")] (cS "" "x")
  outC "cSfloat" 1 rS [("x", VF 1)] (cS "d" "x")
  outC "in3" 1 rF [("3", VF 0.75)] in3
  outC "sec" 1 rF [("_cps", VF 0.5)] (sec (fastFromList [2, 4]))
  outC "msec" 1 rF [("_cps", VF 0.5)] (msec (pure 1000))
  outC "trigger" 2 rS [("_t_pattern", VR 0.25)] (trigger abcd)
  outC "qtrigger" 2 rS [("_t_pattern", VR 0.25)] (qtrigger (slow 4 abcd))
  outC "rtrigger" 2 rS [("_t_pattern", VR 1.6)] (rtrigger (slow 4 abcd))
  outC "ftrigger" 2 rS [("_t_pattern", VR 1.6)] (ftrigger (slow 4 abcd))
  outC "mtrigger" 2 rS [("_t_pattern", VR 5.5)] (mtrigger 4 (slow 4 abcd))
  outC "fastcI" 1 rS [("f", VI 2)] (fast (fromIntegral <$> cI 1 "f") abcd)
  outC "rangecF" 1 rF [("x", VF 0.5)] (segment 2 (range 1 (cF 3 "y") (cF 0 "x")))
  outC "everytrig" 4 rS [("_t_pattern", VR 1)] (every 2 (fast 2) (trigger (slow 2 abcd)))
  outC "juxcF" 1 rC [("p", VF 0.25)] (jux rev (T.s (fastFromList ["a", "b"]) # pan (cF 0 "p")))
