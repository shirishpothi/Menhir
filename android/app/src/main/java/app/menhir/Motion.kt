package app.menhir

import android.animation.ValueAnimator
import android.view.HapticFeedbackConstants
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay

/**
 * Motion for every screen: quick springs that suit the 120 Hz panel. With Remove animations on
 * (animator duration scale 0), Compose already ends its animations at once; [reduceMotion] also
 * skips the delays and the launch overlay, which Compose cannot scale.
 */
fun reduceMotion() = !ValueAnimator.areAnimatorsEnabled()

val LocalReduceMotion = staticCompositionLocalOf { false }

/** The springs used for position and size, and for small things like a key lighting up. */
object Springs {
    fun <T> move() = spring<T>(dampingRatio = 0.86f, stiffness = 420f)
    fun <T> snappy() = spring<T>(dampingRatio = Spring.DampingRatioMediumBouncy, stiffness = Spring.StiffnessMedium)
}

/** Set when the session leaves, so the home screen opens with the Mac's screen shrinking back into its card. */
object HeroLaunch {
    @Volatile var pending = false
}

/** Fades and rises [content] in once, [index] steps after the screen appears. */
@Composable
fun Enter(index: Int, modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    val reduce = LocalReduceMotion.current
    val a = remember { Animatable(if (reduce) 1f else 0f) }
    LaunchedEffect(Unit) {
        if (reduce) return@LaunchedEffect
        delay(45L * index)
        a.animateTo(1f, spring(dampingRatio = 0.9f, stiffness = 300f))
    }
    Box(modifier.graphicsLayer {
        alpha = a.value
        translationY = (1f - a.value) * 28.dp.toPx()
    }) { content() }
}

/** A filled button that dips slightly while pressed and ticks when it acts. */
@Composable
fun PressButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    large: Boolean = false,
    content: @Composable () -> Unit,
) {
    val view = LocalView.current
    val source = remember { MutableInteractionSource() }
    val pressed by source.collectIsPressedAsState()
    val scale by animateFloatAsState(if (pressed) 0.96f else 1f, Springs.snappy(), label = "press")
    Button(
        onClick = { view.performHapticFeedback(HapticFeedbackConstants.CONFIRM); onClick() },
        enabled = enabled,
        interactionSource = source,
        contentPadding = if (large) androidx.compose.foundation.layout.PaddingValues(horizontal = 40.dp) else androidx.compose.material3.ButtonDefaults.ContentPadding,
        modifier = modifier.graphicsLayer { scaleX = scale; scaleY = scale }.heightIn(min = if (large) 64.dp else 48.dp),
    ) {
        if (large) {
            androidx.compose.runtime.CompositionLocalProvider(
                androidx.compose.material3.LocalTextStyle provides MaterialTheme.typography.titleMedium,
            ) { content() }
        } else content()
    }
}

/** A row of choices with one pill that slides to the chosen one. */
@Composable
fun <T> Segmented(label: String, options: List<T>, selected: T, text: (T) -> String, tag: String, enabled: Boolean = true, onSelect: (T) -> Unit) {
    val view = LocalView.current
    val c = MaterialTheme.colorScheme
    Column {
        Text(label, style = MaterialTheme.typography.labelLarge)
        Spacer(Modifier.height(Space.s))
        BoxWithConstraints(
            Modifier.fillMaxWidth().height(48.dp).background(c.surfaceContainerHigh, CircleShape).padding(4.dp).testTag(tag),
        ) {
            val n = options.size.coerceAtLeast(1)
            val w: Dp = maxWidth / n
            val i = options.indexOf(selected).coerceAtLeast(0)
            val x by animateDpAsState(w * i, Springs.move(), label = "segment")
            Box(Modifier.offset { androidx.compose.ui.unit.IntOffset(x.roundToPx(), 0) }.width(w).fillMaxHeight().background(if (enabled) c.primary else c.outline, CircleShape))
            Row(Modifier.fillMaxWidth().fillMaxHeight().selectableGroup()) {
                options.forEach { o ->
                    val on = o == selected
                    val fg by androidx.compose.animation.animateColorAsState(
                        if (on) c.onPrimary else c.onSurface, Springs.move(), label = "segmentText",
                    )
                    Box(
                        Modifier.weight(1f).fillMaxHeight().selectable(on, enabled = enabled, role = Role.RadioButton) {
                            if (!on) {
                                view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                                onSelect(o)
                            }
                        },
                        contentAlignment = Alignment.Center,
                    ) { Text(text(o), style = MaterialTheme.typography.labelLarge, color = fg) }
                }
            }
        }
    }
}
