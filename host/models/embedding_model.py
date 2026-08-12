"""VelaPaw embedding models (host-side, TensorFlow/Keras).

fast     : MobileNetV3-Small -> GAP -> conv-head -> embedding head
accurate : MobileNetV3-Small -> TSFM-Enhanced -> conv-head -> embedding head

TSFM-Enhanced is the operator's thesis module (Task-Specific Feature
Modulation), ported from PyTorch. Original task: multi-task pest ID
(species + stage). Here it is adapted to single-task pet *identity*: the dual
spatial-attention streams residually enrich one shared feature that feeds an
embedding head trained with ArcFace (open-set metric learning). The two streams
are kept (thesis H2) and can specialize to different discriminative regions
(e.g., face vs coat pattern).

Identity-init is preserved: attention final-conv bias = +3 (sigmoid(+3)~=0.953,
near pass-through) and learnable scalars alpha init=0.1, so at init the model is
~ plain GAP and the backbone trains cleanly (graceful degradation: alpha->0
recovers GAP).

On-device (TFLite-Micro) op footprint of the TSFM block: CONV_2D, RELU,
LOGISTIC (sigmoid), MUL, MEAN, ADD -- all TFLM builtins. The full MobileNetV3
backbone adds HARD_SWISH/SE (verify via quantize_export's op list, or use the
minimalistic backbone for a TFLM-safe build).
"""

import tensorflow as tf
from tensorflow.keras import layers, Model
from tensorflow.keras.utils import register_keras_serializable


@register_keras_serializable(package="velapaw")
class HardSwish(layers.Layer):
    """h-swish(x) = x * relu6(x + 3) / 6 (matches MobileNetV3 conv-head).

    A registered Layer (not a function activation) so it serializes cleanly
    through .keras save/load.
    """

    def call(self, x):
        return x * tf.nn.relu6(x + 3.0) / 6.0


# ---------------------------------------------------------------------------
# Embedding-head helpers
# ---------------------------------------------------------------------------
@register_keras_serializable(package="velapaw")
class L2Normalize(layers.Layer):
    """L2-normalize along the last axis (so cosine == dot product)."""

    def call(self, x):
        return tf.math.l2_normalize(x, axis=-1)


# ---------------------------------------------------------------------------
# TSFM-Enhanced (thesis module, ported from PyTorch)
# ---------------------------------------------------------------------------
@register_keras_serializable(package="velapaw")
class SpatialAttention(layers.Layer):
    """Spatial attention: C -> C//8 -> 1, sigmoid; returns x * att.

    Final-conv bias initialised to +3 (identity init; sigmoid(+3)~=0.953).
    """

    def __init__(self, channels, **kw):
        super().__init__(**kw)
        self.channels = channels
        mid = max(channels // 8, 8)
        self.reduce = layers.Conv2D(mid, 1, use_bias=False, name="att_reduce")
        self.act = layers.ReLU()
        self.score = layers.Conv2D(
            1, 1, use_bias=True,
            bias_initializer=tf.keras.initializers.Constant(3.0),
            activation="sigmoid", name="att_score")

    def call(self, x):
        a = self.score(self.act(self.reduce(x)))   # (B,H,W,1) in [0,1]
        return x * a                                # broadcast gate -> (B,H,W,C)

    def get_config(self):
        return {**super().get_config(), "channels": self.channels}


@register_keras_serializable(package="velapaw")
class TSFMEnhanced(layers.Layer):
    """Residual enrichment with two attention streams + learnable scalars.

    enriched = GAP(x) + alpha_a * GAP(att_a(x)) + alpha_b * GAP(att_b(x))
    """

    ALPHA_INIT = 0.1

    def __init__(self, channels, **kw):
        super().__init__(**kw)
        self.channels = channels
        self.att_a = SpatialAttention(channels, name="att_a")  # was species_att
        self.att_b = SpatialAttention(channels, name="att_b")  # was stage_att

    def build(self, input_shape):
        init = tf.keras.initializers.Constant(self.ALPHA_INIT)
        self.alpha_a = self.add_weight(name="alpha_a", shape=(),
                                       initializer=init, trainable=True)
        self.alpha_b = self.add_weight(name="alpha_b", shape=(),
                                       initializer=init, trainable=True)

    def call(self, spatial):
        gap = lambda t: tf.reduce_mean(t, axis=[1, 2])   # (B,C) over H,W
        f_global = gap(spatial)
        f_a = gap(self.att_a(spatial))
        f_b = gap(self.att_b(spatial))
        return f_global + self.alpha_a * f_a + self.alpha_b * f_b

    def get_config(self):
        return {**super().get_config(), "channels": self.channels}


# ---------------------------------------------------------------------------
# Model builder
# ---------------------------------------------------------------------------
def build_embedding_model(variant="fast", input_size=96, channels=3,
                          embed_dim=128, conv_head_dim=1024,
                          alpha=1.0, minimalistic=True):
    """Build the input->embedding model (the part exported to the device).

    variant      : "fast" (GAP) | "accurate" (TSFM-Enhanced enrichment)
    minimalistic : True -> relu, no SE (TFLM-safe); False -> hard-swish + SE
                   (thesis-faithful backbone; verify TFLM op support)
    """
    inp = layers.Input(shape=(input_size, input_size, channels), name="image")

    backbone = tf.keras.applications.MobileNetV3Small(
        input_tensor=inp, alpha=alpha, minimalistic=minimalistic,
        include_top=False, include_preprocessing=False,
        weights="imagenet" if channels == 3 else None)
    spatial = backbone.output                          # (B, h, w, C)
    c = spatial.shape[-1]

    if variant == "accurate":
        x = TSFMEnhanced(c, name="tsfm")(spatial)      # (B, C) enriched
    elif variant == "fast":
        x = layers.GlobalAveragePooling2D(name="gap")(spatial)
    else:
        raise ValueError(f"unknown variant: {variant}")

    # Shared conv-head (matches MobileNetV3 classifier[0:2]: Dense -> h-swish).
    x = layers.Dense(conv_head_dim, name="conv_head")(x)
    x = HardSwish(name="conv_head_act")(x)

    # Embedding head.
    x = layers.Dense(embed_dim, name="embedding_dense")(x)
    x = L2Normalize(name="embedding")(x)
    return Model(inp, x, name=f"velapaw_{variant}")


# ---------------------------------------------------------------------------
# ArcFace margin head (training only)
# ---------------------------------------------------------------------------
@register_keras_serializable(package="velapaw")
class ArcMarginProduct(layers.Layer):
    """ArcFace head: cosine logits with additive angular margin on the target."""

    def __init__(self, num_classes, scale=30.0, margin=0.50, **kw):
        super().__init__(**kw)
        self.num_classes = num_classes
        self.scale = scale
        self.margin = margin

    def build(self, input_shape):
        self.W = self.add_weight(
            name="arc_W", shape=(input_shape[0][-1], self.num_classes),
            initializer="glorot_uniform", trainable=True)

    def call(self, inputs):
        embedding, label = inputs                       # embedding L2-normalized
        w = tf.math.l2_normalize(self.W, axis=0)
        cos = tf.matmul(embedding, w)
        theta = tf.acos(tf.clip_by_value(cos, -1.0 + 1e-7, 1.0 - 1e-7))
        target = tf.cos(theta + self.margin)
        onehot = tf.one_hot(tf.cast(label, tf.int32), self.num_classes)
        return tf.where(tf.cast(onehot, tf.bool), target, cos) * self.scale

    def get_config(self):
        return {**super().get_config(), "num_classes": self.num_classes,
                "scale": self.scale, "margin": self.margin}


def build_training_model(embedding_model, num_classes, scale=30.0, margin=0.50):
    """Wrap the embedding model with an ArcFace head: inputs (image, label)."""
    label = layers.Input(shape=(), dtype="int32", name="label")
    logits = ArcMarginProduct(num_classes, scale, margin, name="arcface")(
        [embedding_model.output, label])
    return Model([embedding_model.input, label], logits, name="train_model")
