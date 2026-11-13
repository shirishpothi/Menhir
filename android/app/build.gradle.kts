// The Menhir app: one Gradle project, which is the application itself. The native side is
// android/native/CMakeLists.txt.
plugins {
    id("com.android.application") version "8.13.2"
    id("org.jetbrains.kotlin.android") version "2.2.21"
    id("org.jetbrains.kotlin.plugin.compose") version "2.2.21"
}

// -PmenhirFakeEngine=false builds against the real engine once its CMake targets exist.
val fakeEngine = (findProperty("menhirFakeEngine") as String?)?.toBoolean() ?: true

android {
    namespace = "app.menhir"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "app.menhir"
        minSdk = 34
        targetSdk = 36
        versionCode = 3
        versionName = "0.1.1"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake { arguments += "-DMENHIR_FAKE_ENGINE=${if (fakeEngine) "ON" else "OFF"}" }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Personal app: the debug key signs release builds too.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    // The helper that runs inside macOS for the clipboard and shared folder, if it has been built
    // on the Mac: make B=build-app build-app/henge-agent.gz (see docs/android/app.md).
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("agent"))

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures { compose = true; buildConfig = true }
    testOptions { unitTests.isReturnDefaultValues = true }
    lint {
        // ColorOS and battery optimisation: asking is the point. Orientation: a landscape tablet app.
        disable += setOf("BatteryLife", "DiscouragedApi", "LockedOrientationActivity", "ChromeOsAbiSupport",
            "GradleDependency", "NewerVersionAvailable", "AndroidGradlePluginVersion")
        // No network access while linting.
        checkDependencies = false
        abortOnError = true
    }
    packaging { jniLibs { useLegacyPackaging = false } }
}

kotlin { jvmToolchain(17) }

val copyAgent by tasks.registering(Copy::class) {
    from(rootDir.resolve("../../build-app/henge-agent.gz"))
    into(layout.buildDirectory.dir("agent"))
}
tasks.named("preBuild") { dependsOn(copyAgent) }

dependencies {
    val bom = platform("androidx.compose:compose-bom:2025.12.01")
    implementation(bom)
    implementation("androidx.core:core-ktx:1.17.0")
    implementation("androidx.activity:activity-compose:1.11.0")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    testImplementation("junit:junit:4.13.2")
}
