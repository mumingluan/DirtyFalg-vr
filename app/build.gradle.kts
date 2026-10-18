plugins {
    id("com.android.application")
}

val nativePayloadDir = providers.gradleProperty("nativePayloadDir").orNull
val localDebugSigning = providers.gradleProperty("localDebugSigning").orNull == "true"

android {
    namespace = "df.root"
    compileSdk = 37

    defaultConfig {
        applicationId = "df.root"
        minSdk = 32
        targetSdk = 37
        versionCode = 47
        versionName = "1.16-vr-logfix.1"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    signingConfigs {
        create("keystore") {
            storeFile = file("keystore.jks")
            storePassword = "dirtyfrag"
            keyAlias = "dirtyfrag"
            keyPassword = "dirtyfrag"
        }
    }

    buildTypes {
        debug {
            signingConfig = signingConfigs.getByName("debug")
        }
        release {
            signingConfig = signingConfigs.getByName(if (localDebugSigning) "debug" else "keystore")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

// AGP 9 removed the old applicationVariants API; this is the replacement.
androidComponents {
    onVariants { variant ->
        variant.outputs.forEach { output ->
            output.outputFileName.set("dirtyfrag.apk")
        }
    }
}
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    lint {
        checkReleaseBuilds = false
    }

    buildFeatures {
        viewBinding = true
        // The toolbar version pill reads BuildConfig.VERSION_NAME, so the pill
        // can never fall behind versionName again.
        buildConfig = true
    }

    if (nativePayloadDir == null) {
        externalNativeBuild {
            cmake {
                path("src/main/jni/CMakeLists.txt")
            }
        }
    } else {
        sourceSets.getByName("main").jniLibs.srcDir(file(nativePayloadDir))
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
            if (nativePayloadDir != null) keepDebugSymbols += "**/*.so"
        }
    }
}

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.constraintlayout)
}
