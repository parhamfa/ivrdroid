plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "ai.rx1.ivrdroid"
    compileSdk = 35

    defaultConfig {
        applicationId = "ai.rx1.ivrdroid"
        minSdk = 29
        targetSdk = 35
        versionCode = 21
        versionName = "0.10.2"

        buildConfigField("String", "CONTROL_PLANE_URL", "\"https://ivrdroid.rx1.ai\"")
        buildConfigField("String", "CONFIG_SIGNING_PUBLIC_KEY_B64", "\"fz+JZNn34uWdo408TosUYVS162AusewBxkg2ip8Cnnw=\"")
        val sourceCommit = providers.environmentVariable("IVRDROID_SOURCE_COMMIT").orNull
            ?.takeIf { it.matches(Regex("[0-9a-f]{40}")) } ?: "unknown"
        buildConfigField("String", "SOURCE_COMMIT", "\"$sourceCommit\"")
        testInstrumentationRunner = "ai.rx1.ivrdroid.control.SessionAuditAcceptanceInstrumentation"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        buildConfig = true
    }

    testOptions {
        unitTests.isReturnDefaultValues = true
    }

    sourceSets.getByName("test").resources.srcDir(rootProject.file("contracts/fixtures"))
}

dependencies {
    implementation("androidx.work:work-runtime:2.11.2")
    implementation("net.i2p.crypto:eddsa:0.3.0")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
}

tasks.withType<Test>().configureEach {
    maxHeapSize = providers.gradleProperty("ivrdroidTestHeap").getOrElse("512m")
}
