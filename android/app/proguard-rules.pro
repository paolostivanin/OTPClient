# OTPClient ProGuard rules

# Keep kotlinx.serialization
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.AnnotationsKt

-keepclassmembers class kotlinx.serialization.json.** {
    *** Companion;
}
-keepclasseswithmembers class kotlinx.serialization.json.** {
    kotlinx.serialization.KSerializer serializer(...);
}

-keep,includedescriptorclasses class com.otpclient.android.**$$serializer { *; }
-keepclassmembers class com.otpclient.android.** {
    *** Companion;
}
-keepclasseswithmembers class com.otpclient.android.** {
    kotlinx.serialization.KSerializer serializer(...);
}

# Bouncy Castle
-keep class org.bouncycastle.** { *; }
-dontwarn org.bouncycastle.**
