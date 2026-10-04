# Code notes: render.neurafi

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/render.neurafi/include/aver/neurafi/NeuraFI.hpp

- **Trajectory modes**: Linear (straight lines, milestone 1 default), Quadratic (quadratic path through three frames with analytic acceleration), Neural (same path with acceleration predicted by MLP network). The network outputs acceleration only, never colour/weight/mask/confidence. Trained in-engine self-supervised on geometric targets from real frames (keeps two more frames of motion history). Neural uses analytic until weights load/warm (kWarmSteps) AND measured error beats analytic (gate).

- **Learning-rate schedule**: Inverse decay over network's lifetime step count (saved with weights, allowing continuation from last session): lr = kLearningRate / (1 + steps / kDecaySteps), floored at kLearningRateFloor. Constant-rate Adam was abandoned when measured error rose from 0.047 to 0.090 px over 2,000 steps at 1e-3.

- **Gate mechanism**: One batch score is noisy (measured: network ranged 0.045-0.180 px against steady 0.11 for quadratic). Both errors smoothed with EMA (kEvalSmoothing), network used only while its smoothed error is below quadratic's after kEvalsToJudge checks. Verdict saved beside weights so non-training sessions know it. Result is never worse than Quadratic on measured data.

## modules/render.neurafi/shaders/neurafi.hlsl

- **CSFgGather pass**: Yang et al. 2011 bidirectional scene reprojection; uses motion field from each real frame's own grid, no scatter, no intermediate motion field. Candidates blended by per-frame confidence. Holes marked confidence 0 for CSFgFill.

- **CSFgFill pass**: Confidence-weighted average of 3x3 neighbours (CONFIDENCE ONLY, never depth-similarity weighted, per US 2025/0106355 patent claim). Seeded by hole's frame-N colour, full resolution, no pyramid. Second pass reaches one pixel further.

- **Patent avoidance**: Georgia Tech US 9,094,660 claims reduce-then-expand pyramid structure; US 2025/0106355 amended claim about depth-similarity weighting in fill.

- **Acceleration image resolution**: gBlock grows with scene (2, 4, 8...) to keep network record count bounded. Measured on 3532x1987 scene with fixed 2x2 block: 1.75M records cost 3.5 ms.

- **Network vs analytic acceleration**: Network predicts correction to v - v', not the whole acceleration. Measured: predicting full `a` could not match analytic error of 0.003-0.018 px on slow wide pans; predicting correction lets network learn "add nothing" where analytic is already exact.

- **Motion-vector seams**: Outliers detected in training as corrections above half the span's own motion. These are edges whose neighbours belong to different surfaces and slipped past depth checks. Measured: a handful of unrejected seams dragged squared-error fit so network did worse than predicting nothing.

- **Training is geometric**: No colour reaches the loss function; network learns motion only from engine motion vectors over three-frame geometry, using frame N-1 as ground truth when interpolating span N-2 -> N.

- **Visualisation modes**: 1=source blend (orange=N, blue=P-1, dimmer=less sure); 2=confidence as heat; 3=path bend magnitude (0.125 |a| px); 4=network share of bend (0.125 |correction| px, zero while quadratic stands in).

## modules/render.neurafi/src/NeuraFI.cpp

- `ensureTrajectory()`: Fresh network output layer is zeroed so gradients can improve on the quadratic. Measured without this: random He-init correction started at 0.47 px against quadratic's 0.087 px and was still worse (0.13 px) 2,500 steps later.

- `generate()`: Network is scored on three-frame check records built exclusively from this session's own frames. Measured why: weights trained on fast jitter scored 0.262 px on slow wide pans where the quadratic reached 0.010 px — a verdict from another motion does not transfer.

- `generate()`: Readback recorded four frames ago has certainly finished because frames in flight are 2.

- `evaluateBatch()`: The three error metrics (linear straight line, analytic acceleration, network correction) are compared as corrections to the analytic path (baseline). The 0.125 factor converts record scale s (via log2(s)/8) to pixel error.

- `evaluateBatch()`: Gate has hysteresis: network enters when it beats quadratic by kGateEnter factor, exits as soon as it is worse. Without hysteresis the path flapped every few checks while the two scores were within noise.

- `evaluateBatch()`: Logged at save cadence (every 5th check) and always after the first evaluation (after only kEvalEverySteps steps of adapting to this session's motion). With loaded weights, the first check is the nearest held-out test of generalization.
